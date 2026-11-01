/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_BTREEFS_OURS_BTREEFS_HPP
#define OURS_BTREEFS_OURS_BTREEFS_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <array>
#include <span>
#include <string_view>
#include <utility>

#include <cstring>

#include <ustl/result.hpp>

/// ours.btreefs, the on-disk filesystem the ours-fs partition carries: one
/// copy-on-write B+tree over fixed 4 KiB blocks, whose format this project
/// defines itself (.agents/tasks/btreefs/DESIGN.md, the COW phase).
///
/// The objects the volume holds:
///   - two superblock slots, blocks 0 and 1: the magic, a generation
///     counter, the CRC32 of the slot's own first bytes, and the volume's
///     state -- the block count, the tree root's block, the tree's height,
///     where the free bitmap lives, the next object id. The slot with the
///     higher sound generation is the live one; a commit writes the other,
///     which is what makes the commit point one block write;
///   - the free bitmap, contiguous blocks from block 2, one bit per block;
///   - B+tree nodes, one block each: a header (leaf flag, entry count,
///     next-leaf pointer) and slots. An internal node keeps child pointer
///     and key together per slot -- child i covers the keys from key i-1 up
///     to but excluding key i; a leaf keeps key and value per slot and
///     chains to the next leaf in key order. What a node's bytes mean, and
///     everything done to them, is detail::Node's to know; the operations
///     ask it and never touch an offset.
///
/// The one tree answers everything. A key names (object, type, offset):
/// an object's INODE item (offset 0) carries its size and kind, a
/// directory's DIRENT items (offset = the name's hash) its children, a
/// file's DATA items (offset = the block's index in the file) its blocks.
/// Lookup, insert, remove and scan are the same code for all of them; only
/// the type byte and the value's meaning differ.
///
/// Nothing is ever modified where it lies. A mutation runs in a
/// Transaction: the node it touches is copied to a fresh block first (the
/// original goes on the transaction's dead list -- the committed tree
/// still names it), the change lands in the copy, and the parent's pointer
/// moves to the copy, the copying climbing to the root. The transaction's
/// cache holds every copy, and reads through it see the transaction's own
/// state; the block device beneath knows nothing of any of it. Blocks
/// freed by the change go on the dead list too, their bits left alone;
/// blocks taken are on the taken list, their bits left alone. commit()
/// then does one thing in a fixed order: write the cached blocks, then
/// the bitmap (taken bits set, dead bits cleared), then the other
/// superblock slot with the next generation. Until that last write the
/// disk holds the committed tree and its bitmap, whole and consistent --
/// a crash is a dropped rehearsal, not a repair. Dropping the transaction
/// without committing is exactly that, on purpose.
///
/// One thread touches a volume (the Loop serializes a server's messages),
/// so none of this locks; the freed blocks becoming reusable at the next
/// transaction is what serializing makes true.
///
/// All fields on disk are little-endian. The library is format logic only,
/// templated over a |Device| whose read_block(index, bytes) and
/// write_block(index, bytes) answer ours::Status and move exactly
/// BLOCK_BYTES bytes -- ours.block's Client behind an adapter on a booted
/// system, a fake over memory in the tests.
namespace ours_btreefs {
/// The byte count of one block, the unit every structure lives in.
inline constexpr auto BLOCK_BYTES = usize{4096};

/// A key's and a value's bytes on disk.
inline constexpr auto KEY_BYTES = usize{16};
inline constexpr auto VALUE_BYTES = usize{64};

/// The most slots of each node kind, from the 16-byte header and a leaf's
/// 80-byte slot (key + value) and an internal node's 24-byte one (child +
/// key), the internal node carrying one child more than it has keys.
inline constexpr auto LEAF_CAPACITY = usize{(BLOCK_BYTES - 16) / (KEY_BYTES + VALUE_BYTES)};
inline constexpr auto INTERNAL_CAPACITY = usize{(BLOCK_BYTES - 16) / (KEY_BYTES + 8) - 1};

/// How few slots a node may carry before a removal must rebalance it: half
/// its capacity, a leaf's 25 items, an internal node's 84 keys. The root is
/// exempt -- it carries whatever the tree needs, one key included.
inline constexpr auto LEAF_MIN = usize{LEAF_CAPACITY / 2};
inline constexpr auto INTERNAL_MIN = usize{INTERNAL_CAPACITY / 2};

/// The most bytes of a DIRENT's name: the 64-byte value holds the child's
/// object id, the kind, the length, and then the name's bytes.
inline constexpr auto MAX_NAME_BYTES = usize{VALUE_BYTES - 8 - 1 - 1};

/// The magic the superblock starts with.
inline constexpr auto MAGIC = std::string_view{"BTREEFS"};

/// What a key's |type| names; the value's meaning follows it.
enum class ItemType : u8 {
    /// The object's metadata, one per object at offset 0.
    Inode = 1,
    /// One child of a directory object, at offset = the child's name hash.
    Dirent = 2,
    /// One block of a file object, at offset = the block's index in the file.
    Data = 3,
};

/// What an object is, in the INODE value and in every DIRENT value.
enum class ObjectKind : u8 {
    File = 1,
    Directory = 2,
};

/// A key: the object it is about, what of it, and where within. The order
/// the tree keeps: by object, then type, then offset.
struct Key {
    u64 object = 0;
    ItemType type = ItemType::Inode;
    /// At most 56 bits wide on disk, the key's second word carrying the
    /// type in its top 8 bits.
    u64 offset = 0;

    [[nodiscard]]
    auto operator<=>(Key const &) const -> std::strong_ordering = default;
};

/// A value, the 64 bytes as they lie; the type's codec builds and reads it.
using Value = std::array<u8, VALUE_BYTES>;

/// An INODE value decoded.
struct InodeValue {
    u64 size = 0;
    ObjectKind kind = ObjectKind::File;
};

/// A DIRENT value decoded; |name| views the value's own bytes.
struct DirentValue {
    u64 child = 0;
    ObjectKind kind = ObjectKind::File;
    std::string_view name;
};

/// Where the tree and the allocator stand: what the superblock carries and
/// what the operations keep current. |height| counts levels, 1 a root that
/// is a leaf and 0 a volume with no tree yet. |next_object| is the next
/// object id a transaction hands out; 0 is no object's id, so a fresh
/// volume starts at 1. |generation| counts committed transactions, the
/// number that picks which superblock slot is the live one.
struct Volume {
    u64 blocks = 0;
    u64 root = 0;
    u8 height = 0;
    u64 bitmap_first = 0;
    u32 bitmap_blocks = 0;
    u64 next_object = 0;
    u64 generation = 0;
};

namespace detail {
    /// The key's second word: the type in the top 8 bits, the offset below.
    constexpr auto OFFSET_BITS = u64{56};
    constexpr auto OFFSET_MASK = u64{(u64{1} << OFFSET_BITS) - 1};

    [[nodiscard]]
    constexpr auto pack_type_offset(ItemType const type, u64 const offset) -> u64 {
        return (u64{std::to_underlying(type)} << OFFSET_BITS) | (offset & OFFSET_MASK);
    }

    [[nodiscard]]
    constexpr auto unpack_type(u64 const word) -> ItemType {
        return static_cast<ItemType>(word >> OFFSET_BITS);
    }

    [[nodiscard]]
    constexpr auto unpack_offset(u64 const word) -> u64 {
        return word & OFFSET_MASK;
    }

    /// The little-endian loads and stores the codecs and the node go through.
    auto load_le16(std::span<u8 const> bytes, usize at) -> u16;
    auto load_le32(std::span<u8 const> bytes, usize at) -> u32;
    auto load_le64(std::span<u8 const> bytes, usize at) -> u64;
    auto store_le16(std::span<u8> bytes, usize at, u16 value) -> void;
    auto store_le32(std::span<u8> bytes, usize at, u32 value) -> void;
    auto store_le64(std::span<u8> bytes, usize at, u64 value) -> void;

    /// The superblock's fields, to and from the block's bytes. The magic is
    /// the caller's to write and to check.
    /// One superblock slot's fields, to the block's bytes: the magic, the
    /// generation, the CRC32 of the slot's first 128 bytes with the CRC
    /// field zeroed, and the volume's state.
    auto store_superblock(std::span<u8> block, Volume const &volume) -> void;

    /// The volume out of a slot's bytes, generation included. NotFound when
    /// the magic is not the volume's, BadState when the CRC does not match
    /// what the slot vouches for.
    auto load_superblock(std::span<u8 const> block) -> ustl::Result<Volume, ours::Status>;

    /// One node's bytes, in place. The view owns nothing -- the bytes are the
    /// caller's buffer -- and the invariants are its: the count is read and
    /// kept current here, the slot arithmetic is private, and no caller sees an
    /// offset. A node read from the device constructs plainly; a fresh one
    /// comes from leaf() or internal() over the caller's zeroed block.
    class Node final {
      public:
        explicit Node(std::span<u8> bytes);

        static auto leaf(std::span<u8> bytes) -> Node;
        static auto internal(std::span<u8> bytes) -> Node;

        [[nodiscard]]
        auto bytes() const -> std::span<u8>;

        [[nodiscard]]
        auto is_leaf() const -> bool;

        [[nodiscard]]
        auto count() const -> usize;

        /// The leaves' chain, a leaf's only.
        [[nodiscard]]
        auto next_leaf() const -> u64;

        auto set_next_leaf(u64 block) -> void;

        [[nodiscard]]
        auto key(usize slot) const -> Key;

        auto set_key(usize slot, Key const &key) -> void;

        /// The child an internal node's slot opens with.
        [[nodiscard]]
        auto child(usize slot) const -> u64;

        auto set_child(usize slot, u64 child) -> void;

        /// The value a leaf's slot keeps behind its key.
        [[nodiscard]]
        auto value(usize slot) const -> Value;

        auto set_value(usize slot, Value const &value) -> void;

        /// How many leading keys are not greater than |key|: the child an
        /// internal node routes by, the slot a leaf inserts at.
        [[nodiscard]]
        auto upper_bound(Key const &key) const -> usize;

        /// The child |key| routes to, an internal node's only.
        [[nodiscard]]
        auto child_for(Key const &key) const -> u64;

        /// Opens a slot at |at| and fills it; the count grows. insert_child()
        /// puts the child after the key's slot, the pair the parent routes by.
        auto insert_leaf(usize at, Key const &key, Value const &value) -> void;
        auto insert_child(usize at, Key const &key, u64 child) -> void;

        /// Closes the slot at |at|; erase_child() takes the key at |at| and the
        /// child at |at| + 1, a merge's leftover pair.
        auto erase_leaf(usize at) -> void;
        auto erase_child(usize at) -> void;

        /// The slot capacity of this node's kind, and the count a removal may
        /// leave a node at before its parent rebalances it. The root is its
        /// caller's business, not the node's.
        [[nodiscard]]
        auto capacity() const -> usize;

        [[nodiscard]]
        auto minimum() const -> usize;

        // --- The split and the rebalance, node to node. The two views are of
        // the same kind; the answers are the separators the parent keeps, or
        // the one a split pushes up. ---

        /// Moves the upper half of |left|'s items into this fresh leaf, which
        /// takes over |left|'s chain link; answers this node's first key, the
        /// separator the parent keeps.
        auto take_upper_half(Node &left) -> Key;

        /// Moves the upper half of |left|'s routing pairs into this fresh
        /// internal node, the pair that split them climbing; answers that key,
        /// which stays in neither node.
        auto take_upper_half_internal(Node &left) -> Key;

        /// Moves |left|'s last item in front of this node's; answers the moved
        /// key, the parent's new separator.
        auto borrow_last_from(Node &left) -> Key;

        /// Moves |left|'s last child -- and the parent's |separator| as this
        /// node's new first key -- into this node, |left|'s old last key
        /// climbing; answers that key.
        auto borrow_last_from_internal(Node &left, Key const &separator) -> Key;

        /// Moves |right|'s first item behind this node's; answers |right|'s new
        /// first key.
        auto borrow_first_from(Node &right) -> Key;

        /// Moves |right|'s first child behind this node's, with the parent's
        /// |separator| as the new last key, |right|'s old first key climbing;
        /// answers that key.
        auto borrow_first_from_internal(Node &right, Key const &separator) -> Key;

        /// Moves every item of |right| behind this node's and hands the chain
        /// link on; |right| is an empty leaf the caller frees.
        auto absorb_leaf(Node &right) -> void;

        /// Absorbs |right| whole, the parent's |separator| descending between
        /// the two; |right| is an empty node the caller frees.
        auto absorb_internal(Node &right, Key const &separator) -> void;

      private:
        std::span<u8> m_bytes;
        bool m_leaf = false;
        usize m_count = 0;

        auto store_count() -> void;

        [[nodiscard]]
        static auto m_leafslot_offset(usize slot) -> usize;

        [[nodiscard]]
        static auto internal_slot_offset(usize slot) -> usize;
    };

    /// The subtree walk the read-like operations share: from the root down to
    /// the leaf |key| routes to, one node at a time on the caller's buffer,
    /// |visit_leaf| answering for the leaf -- a lookup reads it, an update
    /// rewrites it.
    template <typename Device, typename VisitLeaf>
    auto walk(Device &device, Volume const &volume, Key const &key, VisitLeaf &&visit_leaf)
        -> ours::Status {
        auto block = volume.root;
        auto bytes = std::array<u8, BLOCK_BYTES>{};
        for (u8 level = 0; level < volume.height; ++level) {
            if (auto const status = device.read_block(block, std::span{bytes});
                status != ours::Status::Ok) {
                return status;
            }
            auto node = Node{std::span{bytes}};
            if (level + 1 == volume.height) {
                return visit_leaf(node, block);
            }
            block = node.child_for(key);
        }
        return ours::Status::NotFound;
    }
} // namespace detail

/// The hash a DIRENT's offset carries: FNV-1a over the name's bytes, folded
/// to the key's 56-bit offset.
[[nodiscard]]
auto dirent_offset(std::string_view name) -> u64;

auto encode_inode_value(Value &out, u64 size, ObjectKind kind) -> void;

[[nodiscard]]
auto decode_inode_value(Value const &value) -> ustl::Result<InodeValue, ours::Status>;

/// InvArgs when |name| is longer than MAX_NAME_BYTES.
[[nodiscard]]
auto encode_dirent_value(Value &out, u64 child, ObjectKind kind, std::string_view name)
    -> ours::Status;

[[nodiscard]]
auto decode_dirent_value(Value const &value) -> ustl::Result<DirentValue, ours::Status>;

auto encode_data_value(Value &out, u64 block) -> void;

[[nodiscard]]
auto decode_data_value(Value const &value) -> u64;

/// The volume's state over one transaction: a copy of the committed volume
/// the operations mutate, a cache of the blocks they copied or made, and
/// the two lists the commit turns into bitmap bits -- the blocks taken and
/// the blocks dead. Reads through the transaction see its own state; the
/// device beneath knows nothing of any of it.
///
/// The cache is a fixed array of blocks with a linear look-up, sized for
/// one operation's copies: the path down (at most 16 levels), the split's
/// siblings, the rebalance's. An operation past that is BadState, not a
/// wrong answer.
///
/// Dropping the transaction without commit() is the crash rehearsal: the
/// copies die with the object, the committed volume and its bitmap stand,
/// and the blocks the transaction took -- their bits never having left the
/// transaction -- are simply free again.
template <typename Device>
class Transaction final {
  public:
    /// How many blocks one transaction may hold copies of.
    static constexpr auto CACHE_ENTRIES = usize{48};

    Transaction(Device &device, Volume &committed)
        : m_device(device), m_committed(committed), m_shadow(committed) {}

    /// Resets the transaction for another operation on the same storage --
    /// for a caller whose transaction object outlives one operation, whose
    /// cache is too big for a thread's stack to keep constructing. The
    /// state drops exactly as abandonment does.
    auto begin() -> void {
        m_shadow = m_committed;
        m_cache_count = 0;
        m_taken_count = 0;
        m_dead_count = 0;
    }

    Transaction(Transaction const &) = delete;
    auto operator=(Transaction const &) -> Transaction & = delete;

    /// The state the operations mutate, committed to the volume by
    /// commit() and dropped with the transaction otherwise.
    [[nodiscard]]
    auto volume() -> Volume & {
        return m_shadow;
    }

    /// The block's bytes, the transaction's own state: a copy this
    /// transaction made if it holds one, the device's bytes otherwise.
    [[nodiscard]]
    auto cached(u64 block) -> std::span<u8> {
        if (auto const at = find(block); at < m_cache_count) {
            return std::span{m_cache[at].second};
        }
        auto bytes = std::array<u8, BLOCK_BYTES>{};
        auto const status = m_device.read_block(block, bytes);
        if (status != ours::Status::Ok) {
            return {};
        }
        return take(block, bytes);
    }

    /// The block's bytes as the transaction sees them, copied out: the
    /// reads the operations make along their walks.
    auto read_block(u64 block, std::span<u8> bytes) -> ours::Status {
        if (auto const at = find(block); at < m_cache_count) {
            std::memcpy(bytes.data(), m_cache[at].second.data(), BLOCK_BYTES);
            return ours::Status::Ok;
        }
        return m_device.read_block(block, bytes);
    }

    /// Copies |original| into a fresh block: the COW step, whose answer the
    /// parent's pointer moves to and whose original this transaction marks
    /// dead. NotFound from the device passes through.
    [[nodiscard]]
    auto cow(u64 original) -> ustl::Result<u64, ours::Status> {
        // The original's bytes through this transaction's own view: a block
        // an earlier step of the same transaction made exists only in the
        // cache, and copying from the device would copy nothing.
        auto bytes = std::array<u8, BLOCK_BYTES>{};
        if (auto const status = read_block(original, bytes); status != ours::Status::Ok) {
            return ustl::err(status);
        }
        auto const taken = alloc_block();
        if (taken.is_err()) {
            return taken;
        }
        if (take(taken.unwrap(), bytes).empty()) {
            return ustl::err(ours::Status::NoResource);
        }
        auto const marked = mark_dead(original);
        if (marked != ours::Status::Ok) {
            return ustl::err(marked);
        }
        return taken;
    }

    /// Answers one free block, marking it taken -- the bitmap's bit is set
    /// by commit(), not here, so an abandoned transaction's blocks are
    /// simply free again. NoSpace when none is left.
    [[nodiscard]]
    auto alloc_block() -> ustl::Result<u64, ours::Status> {
        for (u32 at = 0; at < m_shadow.bitmap_blocks; ++at) {
            auto bits = std::array<u8, BLOCK_BYTES>{};
            if (auto const status =
                    m_device.read_block(m_shadow.bitmap_first + at, std::span{bits});
                status != ours::Status::Ok) {
                return ustl::err(status);
            }
            for (auto byte = usize{0}; byte < BLOCK_BYTES; ++byte) {
                if (bits[byte] == 0xFF) {
                    continue;
                }
                for (auto bit = usize{0}; bit < 8; ++bit) {
                    auto const block = (u64{at} * BLOCK_BYTES + byte) * 8 + bit;
                    if (block >= m_shadow.blocks) {
                        return ustl::err(ours::Status::NoSpace);
                    }
                    auto const mask = static_cast<u8>(u8{1} << bit);
                    if ((bits[byte] & mask) != 0 || taken_this(block)) {
                        continue;
                    }
                    if (!mark_taken(block)) {
                        return ustl::err(ours::Status::NoResource);
                    }
                    return ustl::ok(block);
                }
            }
        }
        return ustl::err(ours::Status::NoSpace);
    }

    /// Marks |block| dead -- the committed tree names it, the tree this
    /// transaction is building does not. A block this transaction itself
    /// took is not dead but unborn: it leaves the taken list and its cache
    /// entry, free for the next transaction without ever having touched the
    /// bitmap. InvArgs for a block the bitmap does not cover.
    auto free_block(u64 block) -> ours::Status {
        if (block >= m_shadow.blocks ||
            block < m_shadow.bitmap_first + u64{m_shadow.bitmap_blocks}) {
            // Past the volume, or one of the metadata's own -- the
            // superblock slots and the bitmap are never freeable.
            return ours::Status::InvArgs;
        }
        // A block this transaction took is unborn: it leaves the taken list
        // and the cache, free again without ever having touched the bitmap.
        // Anything else was the committed tree's; freeing it twice is the
        // caller's error, BadState as the allocator has always answered.
        for (auto index = usize{0}; index < m_taken_count; ++index) {
            if (m_taken[index] == block) {
                m_taken[index] = m_taken[m_taken_count - 1];
                m_taken_count -= 1;
                drop(block);
                return ours::Status::Ok;
            }
        }
        auto bits = std::array<u8, BLOCK_BYTES>{};
        auto const at = m_shadow.bitmap_first + block / 8 / BLOCK_BYTES;
        if (auto const status = m_device.read_block(at, std::span{bits});
            status != ours::Status::Ok) {
            return status;
        }
        if ((bits[(block / 8) % BLOCK_BYTES] & static_cast<u8>(u8{1} << (block % 8))) == 0) {
            return ours::Status::BadState;
        }
        return mark_dead(block);
    }

    /// Answers the next unused object id and keeps it taken in the
    /// transaction's own state; the superblock writes it at commit.
    auto alloc_object() -> u64 {
        auto const object = m_shadow.next_object;
        m_shadow.next_object += 1;
        return object;
    }

    /// The commit point, in a fixed order: the cached blocks, then the
    /// taken bits set, then the other superblock slot with the next
    /// generation -- and only then the dead bits cleared. The order is the
    /// safety: until the superblock lands, the committed tree is the one
    /// the old generation names, and it still references the dead blocks --
    /// clearing their bits ahead of the swap would mark the live tree's own
    /// nodes free, and the next allocation would write over them. After the
    /// swap the dead blocks are unreferenced, so a crash before their bits
    /// clear only leaks them, and a failed clear here is answered the same
    /// way: the block stays allocated and nothing points at it.
    auto commit() -> ours::Status {
        for (auto index = usize{0}; index < m_cache_count; ++index) {
            if (auto const status =
                    m_device.write_block(m_cache[index].first, m_cache[index].second);
                status != ours::Status::Ok) {
                return status;
            }
        }
        for (auto index = usize{0}; index < m_taken_count; ++index) {
            if (auto const status = bitmap_bit(m_taken[index], true); status != ours::Status::Ok) {
                return status;
            }
        }
        m_shadow.generation = m_committed.generation + 1;
        auto slot = std::array<u8, BLOCK_BYTES>{};
        std::memcpy(slot.data(), MAGIC.data(), MAGIC.size());
        detail::store_superblock(slot, m_shadow);
        auto at = SUPER_FIRST_BLOCK;
        if (m_shadow.generation % 2 == 1) {
            at = SUPER_SECOND_BLOCK;
        }
        if (auto const status = m_device.write_block(at, slot); status != ours::Status::Ok) {
            return status;
        }
        m_committed = m_shadow;
        // The swap is done: the dead blocks are unreferenced now. Their
        // bits clear here; a failure leaks the block (bit set, nothing
        // naming it) and never corrupts -- leak over corruption, always.
        for (auto index = usize{0}; index < m_dead_count; ++index) {
            auto const cleared = bitmap_bit(m_dead[index], false);
            if (cleared != ours::Status::Ok) {
                return cleared;
            }
        }
        return ours::Status::Ok;
    }

  private:
    /// The superblock slots, the bitmap from the block past them.
    static constexpr auto SUPER_FIRST_BLOCK = u64{0};
    static constexpr auto SUPER_SECOND_BLOCK = u64{1};
    static constexpr auto LISTS = usize{64};

    /// The cache entry for |block|, made over |bytes|; the answer is the
    /// entry's own span, the one every later cached() answers.
    auto take(u64 block, std::array<u8, BLOCK_BYTES> const &bytes) -> std::span<u8> {
        if (m_cache_count == CACHE_ENTRIES) {
            return {};
        }
        m_cache[m_cache_count] = {block, bytes};
        m_cache_count += 1;
        return std::span{m_cache[m_cache_count - 1].second};
    }

    [[nodiscard]]
    auto find(u64 block) const -> usize {
        for (auto index = usize{0}; index < m_cache_count; ++index) {
            if (m_cache[index].first == block) {
                return index;
            }
        }
        return CACHE_ENTRIES;
    }

    auto drop(u64 block) -> void {
        auto const at = find(block);
        if (at < m_cache_count) {
            m_cache[at] = m_cache[m_cache_count - 1];
            m_cache_count -= 1;
        }
    }

    [[nodiscard]]
    auto taken_this(u64 block) const -> bool {
        for (auto index = usize{0}; index < m_taken_count; ++index) {
            if (m_taken[index] == block) {
                return true;
            }
        }
        return false;
    }

    auto mark_taken(u64 block) -> bool {
        if (m_taken_count == LISTS) {
            // One transaction's copies and takes outgrew the lists: the
            // caller's operation is refused, nothing on disk having moved.
            return false;
        }
        m_taken[m_taken_count] = block;
        m_taken_count += 1;
        return true;
    }

    auto mark_dead(u64 block) -> ours::Status {
        if (m_dead_count == LISTS) {
            return ours::Status::NoResource;
        }
        m_dead[m_dead_count] = block;
        m_dead_count += 1;
        return ours::Status::Ok;
    }

    /// Sets or clears one block's bit in the bitmap, the block rewritten.
    auto bitmap_bit(u64 block, bool value) -> ours::Status {
        auto const index = block / 8;
        auto bits = std::array<u8, BLOCK_BYTES>{};
        auto const at = m_shadow.bitmap_first + index / BLOCK_BYTES;
        if (auto const status = m_device.read_block(at, std::span{bits});
            status != ours::Status::Ok) {
            return status;
        }
        auto &byte = bits[index % BLOCK_BYTES];
        auto const mask = static_cast<u8>(u8{1} << (block % 8));
        byte =
            value ? static_cast<u8>(byte | mask) : static_cast<u8>(byte & static_cast<u8>(~mask));
        return m_device.write_block(at, std::span{bits});
    }

    Device &m_device;
    Volume &m_committed;
    Volume m_shadow;
    std::array<std::pair<u64, std::array<u8, BLOCK_BYTES>>, CACHE_ENTRIES> m_cache{};
    usize m_cache_count = 0;
    std::array<u64, LISTS> m_taken{};
    usize m_taken_count = 0;
    std::array<u64, LISTS> m_dead{};
    usize m_dead_count = 0;
};

/// Lays a fresh volume on |device|: both superblock slots at generation 1,
/// the free bitmap from block 2, the tree empty. The metadata's own blocks
/// are marked used, so the first allocation lands past them. |blocks| is
/// the volume's whole size, metadata included.
template <typename Device>
auto format(Device &device, u64 blocks, u32 bitmap_blocks) -> ours::Status {
    if (blocks == 0 || bitmap_blocks == 0 || u64{bitmap_blocks} * BLOCK_BYTES * 8 < blocks) {
        return ours::Status::InvArgs;
    }
    auto const volume = Volume{.blocks = blocks,
                               .root = 0,
                               .height = 0,
                               .bitmap_first = 2,
                               .bitmap_blocks = bitmap_blocks,
                               .next_object = 1,
                               .generation = 1};
    auto super = std::array<u8, BLOCK_BYTES>{};
    std::memcpy(super.data(), MAGIC.data(), MAGIC.size());
    detail::store_superblock(super, volume);
    if (auto const status = device.write_block(0, super); status != ours::Status::Ok) {
        return status;
    }
    if (auto const status = device.write_block(1, super); status != ours::Status::Ok) {
        return status;
    }

    // The bitmap: the metadata's blocks marked used in the first bitmap
    // block, the rest starting free -- a block's bit is written once, where
    // the block's number puts it.
    auto bits = std::array<u8, BLOCK_BYTES>{};
    auto const reserved = 2 + u64{bitmap_blocks};
    for (auto block = u64{0}; block < reserved; ++block) {
        bits[block / 8] |= static_cast<u8>(u8{1} << (block % 8));
    }
    for (u32 at = 0; at < bitmap_blocks; ++at) {
        if (auto const status = device.write_block(2 + at, std::span{bits});
            status != ours::Status::Ok) {
            return status;
        }
        bits = {};
    }
    return ours::Status::Ok;
}

/// Reads the volume's live superblock: both slots, the one with the higher
/// generation whose CRC matches. NotFound when neither carries the magic,
/// BadState when they carry it and neither survives its own CRC.
template <typename Device>
auto open(Device &device) -> ustl::Result<Volume, ours::Status> {
    auto best = ustl::Result<Volume, ours::Status>(ustl::err(ours::Status::NotFound));
    for (auto const at: {u64{0}, u64{1}}) {
        auto slot = std::array<u8, BLOCK_BYTES>{};
        if (auto const status = device.read_block(at, std::span{slot});
            status != ours::Status::Ok) {
            return best;
        }
        auto const loaded = detail::load_superblock(slot);
        if (loaded.is_err()) {
            // A slot with the magic but a broken CRC means the volume is
            // here and damaged; say BadState unless the other slot is
            // sound, which it is when it answered.
            if (loaded.unwrap_err() == ours::Status::BadState && best.is_err() &&
                best.unwrap_err() == ours::Status::NotFound) {
                best = ustl::err(ours::Status::BadState);
            }
            continue;
        }
        if (best.is_err() || loaded->generation > best->generation) {
            best = loaded;
        }
    }
    return best;
}

/// Finds |key| and copies its value into |value|, reading the committed
/// tree. NotFound when no item carries it -- an empty volume included.
template <typename Device>
auto lookup(Device &device, Volume const &volume, Key const &key, Value &value) -> ours::Status {
    auto block = volume.root;
    auto bytes = std::array<u8, BLOCK_BYTES>{};
    for (u8 level = 0; level < volume.height; ++level) {
        if (auto const status = device.read_block(block, bytes); status != ours::Status::Ok) {
            return status;
        }
        auto node = detail::Node{bytes};
        if (level + 1 == volume.height) {
            auto const at = node.upper_bound(key);
            if (at == 0 || node.key(at - 1) != key) {
                return ours::Status::NotFound;
            }
            value = node.value(at - 1);
            return ours::Status::Ok;
        }
        block = node.child_for(key);
    }
    return ours::Status::NotFound;
}

/// Calls |visit| on every item from |from| onward, in key order, walking
/// the tree from its root in order -- the leaves' chain is not trusted, a
/// copy-on-write split copying a leaf without reaching back to the
/// predecessor whose next pointer names the block the copy replaced.
/// |visit| takes (Key const &, Value const &) and answers bool; false ends
/// the walk.
template <typename Device, typename Visit>
auto scan(Device &device, Volume const &volume, Key const &from, Visit &&visit) -> ours::Status {
    if (volume.height == 0) {
        return ours::Status::Ok;
    }
    // The recursive in-order walk, pruned: a child whose separator is past
    // |from|... rather, one whose keys are all before |from| -- the child's
    // range ends at its separator -- is skipped whole.
    // (The helper below carries the recursion.)
    struct Walk {
        Device &device;
        Visit &visit;
        Key const &from;
        bool started = false;
        bool stop = false;

        auto run(u64 block, u8 height) -> ours::Status {
            auto bytes = std::array<u8, BLOCK_BYTES>{};
            if (auto const status = device.read_block(block, bytes); status != ours::Status::Ok) {
                return status;
            }
            auto node = detail::Node{bytes};
            if (height == 1) {
                for (auto slot = usize{0}; slot < node.count() && !stop; ++slot) {
                    auto const key = node.key(slot);
                    if (!started) {
                        if (key < from) {
                            continue;
                        }
                        started = true;
                    }
                    if (!visit(key, node.value(slot))) {
                        stop = true;
                    }
                }
                return ours::Status::Ok;
            }
            for (auto slot = usize{0}; slot <= node.count() && !stop; ++slot) {
                if (slot < node.count() && node.key(slot) < from && started) {
                    // The child's whole range sits before |from|.
                    continue;
                }
                if (auto const status = run(node.child(slot), static_cast<u8>(height - 1));
                    status != ours::Status::Ok) {
                    return status;
                }
            }
            return ours::Status::Ok;
        }
    };
    auto walk = Walk{device, visit, from};
    if (auto const status = walk.run(volume.root, volume.height); status != ours::Status::Ok) {
        return status;
    }
    return ours::Status::Ok;
}

/// Replaces the value at |key| where the transaction's copy of its leaf
/// lies -- the leaf copied, the path to it with it, every original on the
/// dead list. NotFound when no item carries the key.
template <typename Device>
auto update(Transaction<Device> &tx, Key const &key, Value const &value) -> ours::Status {
    auto &volume = tx.volume();
    if (volume.height == 0) {
        return ours::Status::NotFound;
    }
    auto copy = tx.cow(volume.root);
    if (copy.is_err()) {
        return copy.unwrap_err();
    }
    auto top = copy.unwrap();
    for (u8 level = 0; level + 1 < volume.height; ++level) {
        auto node = detail::Node{tx.cached(copy.unwrap())};
        auto const child = node.child_for(key);
        auto const copied = tx.cow(child);
        if (copied.is_err()) {
            return copied.unwrap_err();
        }
        node.set_child(node.upper_bound(key), copied.unwrap());
        copy = copied;
    }
    auto node = detail::Node{tx.cached(copy.unwrap())};
    auto const at = node.upper_bound(key);
    if (at == 0 || node.key(at - 1) != key) {
        return ours::Status::NotFound;
    }
    node.set_value(at - 1, value);
    volume.root = top;
    return ours::Status::Ok;
}

/// Inserts |key| with |value|, copying every node on its path and splitting
/// what has to split; the superblock is written by commit(), which the
/// caller runs when the operation is done. AlreadyExists when an item with
/// the key is there; NoSpace when a split finds no block; InvArgs when the
/// key's offset does not fit the 56 bits.
template <typename Device>
auto insert(Transaction<Device> &tx, Key const &key, Value const &value) -> ours::Status {
    if (key.offset > detail::OFFSET_MASK) {
        return ours::Status::InvArgs;
    }
    auto &volume = tx.volume();

    // An empty volume grows its first leaf and falls through to the one
    // path every insert takes.
    if (volume.height == 0) {
        auto const leaf = tx.alloc_block();
        if (leaf.is_err()) {
            return leaf.unwrap_err();
        }
        detail::Node::leaf(tx.cached(leaf.unwrap()));
        volume.root = leaf.unwrap();
        volume.height = 1;
    }

    // The path down, every node copied on the way: the copy is where the
    // change will land, the parent's pointer moving to it, the original on
    // the dead list. The copies are what the walk holds.
    auto path = std::array<u64, 16>{};
    if (u64{volume.height} > path.size()) {
        return ours::Status::BadState;
    }
    auto root = tx.cow(volume.root);
    if (root.is_err()) {
        return root.unwrap_err();
    }
    path[0] = root.unwrap();
    for (u8 level = 0; level + 1 < volume.height; ++level) {
        auto node = detail::Node{tx.cached(path[level])};
        auto const child = node.child_for(key);
        auto const copied = tx.cow(child);
        if (copied.is_err()) {
            return copied.unwrap_err();
        }
        node.set_child(node.upper_bound(key), copied.unwrap());
        path[level + 1] = copied.unwrap();
    }
    // The tree's root is the walk's topmost copy from here on; commit()
    // writes it, and an abandoned transaction drops it with everything else.
    volume.root = path[0];

    auto leaf = detail::Node{tx.cached(path[volume.height - 1])};
    auto const at = leaf.upper_bound(key);
    if (at > 0 && leaf.key(at - 1) == key) {
        return ours::Status::AlreadyExists;
    }

    if (leaf.count() < leaf.capacity()) {
        leaf.insert_leaf(at, key, value);
        return ours::Status::Ok;
    }

    // The leaf is full: split it, put the item into whichever half its key
    // sorts into, and send the separator up.
    auto right_block = tx.alloc_block();
    if (right_block.is_err()) {
        return right_block.unwrap_err();
    }
    auto right = detail::Node::leaf(tx.cached(right_block.unwrap()));
    auto const separator = right.take_upper_half(leaf);
    if (key < separator) {
        leaf.insert_leaf(leaf.upper_bound(key), key, value);
    } else {
        right.insert_leaf(right.upper_bound(key), key, value);
    }
    // The chain: the right leaf took |leaf|'s old link in the split; the
    // left one now points at the right.
    leaf.set_next_leaf(right_block.unwrap());

    // The pair climbs one level at a time, each level splitting the same
    // way, until a level takes it -- or every level has split, and a new
    // root above them is the tree growing.
    auto up_key = separator;
    auto up_child = right_block.unwrap();
    auto level = u8{static_cast<u8>(volume.height - 1)};
    while (level > 0) {
        --level;
        auto node = detail::Node{tx.cached(path[level])};
        auto const at = node.upper_bound(up_key);
        if (node.count() < node.capacity()) {
            node.insert_child(at, up_key, up_child);
            return ours::Status::Ok;
        }
        auto right_node_block = tx.alloc_block();
        if (right_node_block.is_err()) {
            return right_node_block.unwrap_err();
        }
        auto right_node = detail::Node::internal(tx.cached(right_node_block.unwrap()));
        auto const mid = right_node.take_upper_half_internal(node);
        if (up_key < mid) {
            node.insert_child(node.upper_bound(up_key), up_key, up_child);
        } else {
            right_node.insert_child(right_node.upper_bound(up_key), up_key, up_child);
        }
        up_key = mid;
        up_child = right_node_block.unwrap();
    }

    auto const new_root = tx.alloc_block();
    if (new_root.is_err()) {
        return new_root.unwrap_err();
    }
    auto root_node = detail::Node::internal(tx.cached(new_root.unwrap()));
    root_node.insert_child(0, up_key, up_child);
    // The new root's first child is the copied root, the pair's left side.
    root_node.set_child(0, path[0]);
    volume.root = new_root.unwrap();
    volume.height = static_cast<u8>(volume.height + 1);
    return ours::Status::Ok;
}

namespace detail {
    /// Fixes the underflowed child at |index| of the internal node |node|: a
    /// borrow from a sibling that can spare, otherwise a merge whose routing
    /// pair leaves the node. Every node touched here is copied first -- the
    /// child already was, by the recursion -- and the freed one goes to the
    /// transaction, its dead original with it. |underflow| answers whether the
    /// node itself now carries fewer than its minimum.
    template <typename Device>
    auto fix_child(Transaction<Device> &tx, Node &node, usize const index, bool &underflow)
        -> ours::Status {
        auto cur_block = node.child(index);
        auto cur = Node{tx.cached(cur_block)};
        auto const min = cur.minimum();

        // The siblings are copied up front, whatever the rebalance turns out to
        // be -- a borrow moves bytes in them, a merge ends one of them.
        auto left_block = u64{0};
        auto have_left = false;
        if (index > 0) {
            auto const copied = tx.cow(node.child(index - 1));
            if (copied.is_err()) {
                return copied.unwrap_err();
            }
            left_block = copied.unwrap();
            node.set_child(index - 1, left_block);
            have_left = true;
        }
        auto right_block = u64{0};
        auto have_right = false;
        if (index < node.count()) {
            auto const copied = tx.cow(node.child(index + 1));
            if (copied.is_err()) {
                return copied.unwrap_err();
            }
            right_block = copied.unwrap();
            node.set_child(index + 1, right_block);
            have_right = true;
        }

        // The left sibling first: it lends its last, and the separator between
        // it and the child becomes what the child now starts with.
        if (have_left) {
            auto left = Node{tx.cached(left_block)};
            if (left.count() > min) {
                auto const separator =
                    cur.is_leaf() ? cur.borrow_last_from(left)
                                  : cur.borrow_last_from_internal(left, node.key(index - 1));
                node.set_key(index - 1, separator);
                underflow = node.count() < node.minimum();
                return ours::Status::Ok;
            }
        }

        // The right sibling next, the same the other way.
        if (have_right) {
            auto right = Node{tx.cached(right_block)};
            if (right.count() > min) {
                auto const separator = cur.is_leaf()
                                           ? cur.borrow_first_from(right)
                                           : cur.borrow_first_from_internal(right, node.key(index));
                node.set_key(index, separator);
                underflow = node.count() < node.minimum();
                return ours::Status::Ok;
            }
        }

        // No sibling can spare: the merge. The left one is preferred, so the
        // child slides into what precedes it and dies; with no left, the right
        // slides into the child.
        if (have_left) {
            auto left = Node{tx.cached(left_block)};
            if (cur.is_leaf()) {
                left.absorb_leaf(cur);
            } else {
                left.absorb_internal(cur, node.key(index - 1));
            }
            if (auto const status = tx.free_block(cur_block); status != ours::Status::Ok) {
                return status;
            }
            node.erase_child(index - 1);
        } else {
            auto right = Node{tx.cached(right_block)};
            if (cur.is_leaf()) {
                cur.absorb_leaf(right);
            } else {
                cur.absorb_internal(right, node.key(0));
            }
            if (auto const status = tx.free_block(right_block); status != ours::Status::Ok) {
                return status;
            }
            node.erase_child(0);
        }
        underflow = node.count() < node.minimum();
        return ours::Status::Ok;
    }

    /// Removes the key from the subtree |block| roots at |height| levels,
    /// answering |underflow| and the copy's block -- the parent's pointer moves
    /// to it. The parent's turn happens one frame up, which is what makes the
    /// cascade: a merge there removes a routing pair, and the same rule applies
    /// again.
    template <typename Device>
    auto remove_from(Transaction<Device> &tx, u64 block, u8 height, Key const &key, bool &underflow,
                     u64 &new_block) -> ours::Status {
        auto const copied = tx.cow(block);
        if (copied.is_err()) {
            return copied.unwrap_err();
        }
        new_block = copied.unwrap();
        auto node = detail::Node{tx.cached(new_block)};
        if (height == 1) {
            auto const at = node.upper_bound(key);
            if (at == 0 || node.key(at - 1) != key) {
                return ours::Status::NotFound;
            }
            node.erase_leaf(at - 1);
            underflow = node.count() < node.minimum();
            return ours::Status::Ok;
        }
        auto const index = node.upper_bound(key);
        auto child_new = u64{0};
        auto child_under = false;
        if (auto const status = remove_from(tx, node.child(index), static_cast<u8>(height - 1), key,
                                            child_under, child_new);
            status != ours::Status::Ok) {
            return status;
        }
        node.set_child(index, child_new);
        if (!child_under) {
            underflow = false;
            return ours::Status::Ok;
        }
        return fix_child(tx, node, index, underflow);
    }
} // namespace detail

/// Removes every DATA item of |object| at |from_block| and beyond, each
/// item's block back to the transaction as it goes; the object's INODE is
/// untouched -- the caller sizes it. The items leave one at a time, a
/// crash dropping the whole transaction. NotFound answers Ok: a file with
/// no blocks past |from_block| is already as short as asked.
template <typename Device>
auto truncate(Transaction<Device> &tx, u64 object, u64 from_block) -> ours::Status {
    while (true) {
        // The first DATA item at |from_block| or past: the walk starts
        // there, skips the INODE item sorting before the DATA ones, and
        // stops at the next object's items.
        auto block_key = Key{};
        auto seen = false;
        auto const walked =
            scan(tx, tx.volume(), {.object = object, .type = ItemType::Data, .offset = from_block},
                 [&](Key const &key, Value const &) -> bool {
                     if (key.object != object) {
                         return false;
                     }
                     if (key.type != ItemType::Data) {
                         return key.offset < from_block;
                     }
                     block_key = key;
                     seen = true;
                     return false;
                 });
        if (walked != ours::Status::Ok) {
            return walked;
        }
        if (!seen) {
            return ours::Status::Ok;
        }
        auto item = Value{};
        if (auto const status = lookup(tx, tx.volume(), block_key, item);
            status != ours::Status::Ok) {
            return status;
        }
        if (auto const status = remove(tx, block_key); status != ours::Status::Ok) {
            return status;
        }
        if (auto const status = tx.free_block(decode_data_value(item));
            status != ours::Status::Ok) {
            return status;
        }
    }
}

/// Removes the item |key| names, rebalancing what the removal leaves past
/// its minimum: a leaf borrows from a sibling, or merges with one -- the
/// merge taking the routing pair out of the parent, which may underflow in
/// its turn -- and a root left with one child is replaced by that child,
/// the tree a level shorter. Every node the old tree named along the way is
/// on the dead list, its block back at commit. NotFound when no item
/// carries the key.
template <typename Device>
auto remove(Transaction<Device> &tx, Key const &key) -> ours::Status {
    auto &volume = tx.volume();
    if (volume.height == 0) {
        return ours::Status::NotFound;
    }
    auto underflow = false;
    auto new_root = u64{0};
    if (auto const status =
            detail::remove_from(tx, volume.root, volume.height, key, underflow, new_root);
        status != ours::Status::Ok) {
        return status;
    }
    volume.root = new_root;
    // The root never borrows -- it carries what the tree needs -- but an
    // internal root left with a single child is a level for nothing: the
    // child rises, the copy's block goes back.
    while (volume.height > 1) {
        auto node = detail::Node{tx.cached(volume.root)};
        if (node.count() != 0) {
            break;
        }
        auto const child = node.child(0);
        if (auto const status = tx.free_block(volume.root); status != ours::Status::Ok) {
            return status;
        }
        volume.root = child;
        volume.height = static_cast<u8>(volume.height - 1);
    }
    return ours::Status::Ok;
}
} // namespace ours_btreefs

#endif // #ifndef OURS_BTREEFS_OURS_BTREEFS_HPP
