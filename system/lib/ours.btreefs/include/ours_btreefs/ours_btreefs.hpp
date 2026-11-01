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
/// B+tree over fixed 4 KiB blocks, updated in place, whose format this
/// project defines itself (.agents/tasks/btreefs/DESIGN.md).
///
/// The objects the volume holds:
///   - the superblock, block 0: the magic, the block count, the tree root's
///     block, the tree's height, where the free bitmap lives, and the next
///     object id;
///   - the free bitmap, contiguous blocks from block 1, one bit per block;
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
/// The events of an insert: descend from the root, at each internal node
/// taking the child whose range covers the key; at the leaf, make room and
/// slot the item in -- or, the leaf full, split it into a right sibling
/// whose first key becomes the separator pushed into the parent, the parent
/// splitting the same way, a root's split growing the tree one level. The
/// events of a remove are the mirror: a node left past its minimum borrows
/// from a sibling that can spare, or merges with one -- the merge taking a
/// routing pair out of the parent, which may underflow in its turn -- and a
/// root with one child is replaced by that child. The separator is a
/// routing copy: the item itself stays in a leaf, and a lookup for the
/// separator's key must land in the right sibling, which is why the child
/// an internal node routes to is the one past the last key that is not
/// greater than the target.
///
/// Updates are in place: a crash mid-insert can corrupt the tree, accepted
/// for v1 and revisited as copy-on-write. One thread touches a volume (the
/// Loop serializes a server's messages), so none of this locks.
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
/// object id alloc_object() hands out; 0 is no object's id, so a fresh
/// volume starts at 1.
struct Volume {
    u64 blocks = 0;
    u64 root = 0;
    u8 height = 0;
    u64 bitmap_first = 0;
    u32 bitmap_blocks = 0;
    u64 next_object = 0;
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
    auto store_superblock(std::span<u8> block, Volume const &volume) -> void;
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

    /// Rebalances the underflowed child |key| routes to of the internal node
    /// |node|: a borrow from a sibling that can spare, otherwise a merge whose
    /// routing pair leaves the node. |underflow| answers whether the node
    /// itself now carries fewer than its minimum. Defined beside remove().
    template <typename Device>
    auto fix_child(Device &device, Volume &volume, Node &node, usize index, bool &underflow)
        -> ours::Status;

    /// The recursive half of remove(): the key out of the subtree |block|
    /// roots at |height| levels, and the rebalance on the way back up.
    template <typename Device>
    auto remove_from(Device &device, Volume &volume, u64 block, u8 height, Key const &key,
                     bool &underflow) -> ours::Status;

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

/// Lays a fresh volume on |device|: the superblock at block 0, the free
/// bitmap in the |bitmap_blocks| blocks from block 1, the tree empty. The
/// metadata's own blocks are marked used, so the first allocation lands
/// past them. |blocks| is the volume's whole size, metadata included.
template <typename Device>
auto format(Device &device, u64 blocks, u32 bitmap_blocks) -> ours::Status {
    if (blocks == 0 || bitmap_blocks == 0 || u64{bitmap_blocks} * BLOCK_BYTES * 8 < blocks) {
        return ours::Status::InvArgs;
    }
    auto super = std::array<u8, BLOCK_BYTES>{};
    std::memcpy(super.data(), MAGIC.data(), MAGIC.size());
    detail::store_superblock(super, Volume{.blocks = blocks,
                                           .root = 0,
                                           .height = 0,
                                           .bitmap_first = 1,
                                           .bitmap_blocks = bitmap_blocks,
                                           .next_object = 1});
    if (auto const status = device.write_block(0, std::span{super}); status != ours::Status::Ok) {
        return status;
    }

    // The bitmap: the metadata's blocks marked used in the first bitmap
    // block, the rest starting free -- a block's bit is written once, where
    // the block's number puts it.
    auto bits = std::array<u8, BLOCK_BYTES>{};
    auto const reserved = 1 + u64{bitmap_blocks};
    for (auto block = u64{0}; block < reserved; ++block) {
        bits[block / 8] |= static_cast<u8>(u8{1} << (block % 8));
    }
    for (u32 at = 0; at < bitmap_blocks; ++at) {
        if (auto const status = device.write_block(1 + at, std::span{bits});
            status != ours::Status::Ok) {
            return status;
        }
        bits = {};
    }
    return ours::Status::Ok;
}

/// Writes the volume's state back to the superblock. The operations that
/// change root, height or the object counter call it; the caller persists
/// nothing.
template <typename Device>
auto persist_super(Device &device, Volume const &volume) -> ours::Status {
    auto super = std::array<u8, BLOCK_BYTES>{};
    if (auto const status = device.read_block(0, std::span{super}); status != ours::Status::Ok) {
        return status;
    }
    detail::store_superblock(super, volume);
    return device.write_block(0, std::span{super});
}

/// Reads the superblock of a formatted volume. NotFound when block 0 does
/// not carry the magic.
template <typename Device>
auto open(Device &device) -> ustl::Result<Volume, ours::Status> {
    auto super = std::array<u8, BLOCK_BYTES>{};
    if (auto const status = device.read_block(0, std::span{super}); status != ours::Status::Ok) {
        return ustl::err(status);
    }
    return detail::load_superblock(super);
}

/// Answers the next unused object id and keeps it taken: the superblock is
/// written before the id is used, so a crash can only waste one.
template <typename Device>
auto alloc_object(Device &device, Volume &volume) -> u64 {
    auto const object = volume.next_object;
    volume.next_object += 1;
    auto _ = persist_super(device, volume);
    return object;
}

/// Answers one free block, marking it used. NoSpace when none is left.
template <typename Device>
auto alloc_block(Device &device, Volume &volume) -> ustl::Result<u64, ours::Status> {
    for (u32 at = 0; at < volume.bitmap_blocks; ++at) {
        auto bits = std::array<u8, BLOCK_BYTES>{};
        if (auto const status = device.read_block(volume.bitmap_first + at, std::span{bits});
            status != ours::Status::Ok) {
            return ustl::err(status);
        }
        for (auto byte = usize{0}; byte < BLOCK_BYTES; ++byte) {
            if (bits[byte] == 0xFF) {
                continue;
            }
            for (auto bit = usize{0}; bit < 8; ++bit) {
                auto const block = (u64{at} * BLOCK_BYTES + byte) * 8 + bit;
                if (block >= volume.blocks) {
                    return ustl::err(ours::Status::NoSpace);
                }
                auto const mask = static_cast<u8>(u8{1} << bit);
                if ((bits[byte] & mask) != 0) {
                    continue;
                }
                bits[byte] |= mask;
                if (auto const status =
                        device.write_block(volume.bitmap_first + at, std::span{bits});
                    status != ours::Status::Ok) {
                    return ustl::err(status);
                }
                return ustl::ok(block);
            }
        }
    }
    return ustl::err(ours::Status::NoSpace);
}

/// Marks |block| free again. InvArgs for a block the bitmap does not cover
/// or one of the metadata's own -- the superblock and the bitmap are never
/// freeable -- BadState for one already free.
template <typename Device>
auto free_block(Device &device, Volume &volume, u64 block) -> ours::Status {
    auto const index = block / 8;
    if (block >= volume.blocks || index >= u64{volume.bitmap_blocks} * BLOCK_BYTES ||
        block < volume.bitmap_first + u64{volume.bitmap_blocks}) {
        return ours::Status::InvArgs;
    }
    auto bits = std::array<u8, BLOCK_BYTES>{};
    auto const at = volume.bitmap_first + index / BLOCK_BYTES;
    if (auto const status = device.read_block(at, std::span{bits}); status != ours::Status::Ok) {
        return status;
    }
    auto &byte = bits[index % BLOCK_BYTES];
    auto const mask = static_cast<u8>(u8{1} << (block % 8));
    if ((byte & mask) == 0) {
        return ours::Status::BadState;
    }
    byte &= static_cast<u8>(~mask);
    return device.write_block(at, std::span{bits});
}

/// Finds |key| and copies its value into |value|. NotFound when no item
/// carries it -- an empty volume included.
template <typename Device>
auto lookup(Device &device, Volume const &volume, Key const &key, Value &value) -> ours::Status {
    return detail::walk(device, volume, key, [&](detail::Node &node, u64) -> ours::Status {
        auto const at = node.upper_bound(key);
        if (at == 0 || node.key(at - 1) != key) {
            return ours::Status::NotFound;
        }
        value = node.value(at - 1);
        return ours::Status::Ok;
    });
}

/// Replaces the value at |key| where it lies -- no structure changes, the
/// key and its neighbours untouched. NotFound when no item carries it.
template <typename Device>
auto update(Device &device, Volume const &volume, Key const &key, Value const &value)
    -> ours::Status {
    return detail::walk(device, volume, key, [&](detail::Node &node, u64 block) -> ours::Status {
        auto const at = node.upper_bound(key);
        if (at == 0 || node.key(at - 1) != key) {
            return ours::Status::NotFound;
        }
        node.set_value(at - 1, value);
        return device.write_block(block, node.bytes());
    });
}

/// Inserts |key| with |value|, splitting what has to split and growing the
/// tree as it goes; the superblock is written when root or height change.
/// AlreadyExists when an item with the key is there; NoSpace when a split
/// finds no block; InvArgs when the key's offset does not fit the 56 bits.
template <typename Device>
auto insert(Device &device, Volume &volume, Key const &key, Value const &value) -> ours::Status {
    if (key.offset > detail::OFFSET_MASK) {
        return ours::Status::InvArgs;
    }

    // An empty volume grows its first leaf and falls through to the one
    // path every insert takes.
    if (volume.height == 0) {
        auto const leaf = alloc_block(device, volume);
        if (leaf.is_err()) {
            return leaf.unwrap_err();
        }
        auto bytes = std::array<u8, BLOCK_BYTES>{};
        detail::Node::leaf(std::span{bytes});
        if (auto const status = device.write_block(leaf.unwrap(), bytes);
            status != ours::Status::Ok) {
            return status;
        }
        volume.root = leaf.unwrap();
        volume.height = 1;
        if (auto const status = persist_super(device, volume); status != ours::Status::Ok) {
            return status;
        }
    }

    // The path down, block numbers only: the nodes are read again on the
    // walk back up that a split's separator push is, no one else having
    // written them meanwhile (one thread serves the volume). Buffers on the
    // path would cost 4 KiB each per level; this thread's stack is small.
    auto path = std::array<u64, 16>{};
    if (u64{volume.height} > path.size()) {
        return ours::Status::BadState;
    }
    path[0] = volume.root;
    auto bytes = std::array<u8, BLOCK_BYTES>{};
    for (u8 level = 0; level + 1 < volume.height; ++level) {
        if (auto const status = device.read_block(path[level], bytes); status != ours::Status::Ok) {
            return status;
        }
        path[level + 1] = detail::Node{bytes}.child_for(key);
    }

    auto m_leafbytes = std::array<u8, BLOCK_BYTES>{};
    if (auto const status = device.read_block(path[volume.height - 1], m_leafbytes);
        status != ours::Status::Ok) {
        return status;
    }
    auto leaf = detail::Node{m_leafbytes};
    auto const at = leaf.upper_bound(key);
    if (at > 0 && leaf.key(at - 1) == key) {
        return ours::Status::AlreadyExists;
    }

    if (leaf.count() < leaf.capacity()) {
        leaf.insert_leaf(at, key, value);
        return device.write_block(path[volume.height - 1], leaf.bytes());
    }

    // The leaf is full: split it, put the item into whichever half its key
    // sorts into, and send the separator up.
    auto right_bytes = std::array<u8, BLOCK_BYTES>{};
    auto right = detail::Node::leaf(right_bytes);
    auto const separator = right.take_upper_half(leaf);
    if (key < separator) {
        leaf.insert_leaf(leaf.upper_bound(key), key, value);
    } else {
        right.insert_leaf(right.upper_bound(key), key, value);
    }
    auto const right_block = alloc_block(device, volume);
    if (right_block.is_err()) {
        return right_block.unwrap_err();
    }
    // The chain: the right leaf took |leaf|'s old link in the split; the
    // left one now points at the right.
    leaf.set_next_leaf(right_block.unwrap());
    if (auto const status = device.write_block(path[volume.height - 1], leaf.bytes());
        status != ours::Status::Ok) {
        return status;
    }
    if (auto const status = device.write_block(right_block.unwrap(), right.bytes());
        status != ours::Status::Ok) {
        return status;
    }

    // The pair climbs one level at a time, each level splitting the same
    // way, until a level takes it -- or every level has split, and a new
    // root above them is the tree growing.
    auto up_key = separator;
    auto up_child = right_block.unwrap();
    auto level = u8{static_cast<u8>(volume.height - 1)};
    while (level > 0) {
        --level;
        if (auto const status = device.read_block(path[level], bytes); status != ours::Status::Ok) {
            return status;
        }
        auto node = detail::Node{bytes};
        auto const at = node.upper_bound(up_key);
        if (node.count() < node.capacity()) {
            node.insert_child(at, up_key, up_child);
            return device.write_block(path[level], node.bytes());
        }
        auto right_node_bytes = std::array<u8, BLOCK_BYTES>{};
        auto right_node = detail::Node::internal(right_node_bytes);
        auto const mid = right_node.take_upper_half_internal(node);
        if (up_key < mid) {
            node.insert_child(node.upper_bound(up_key), up_key, up_child);
        } else {
            right_node.insert_child(right_node.upper_bound(up_key), up_key, up_child);
        }
        auto const right = alloc_block(device, volume);
        if (right.is_err()) {
            return right.unwrap_err();
        }
        if (auto const status = device.write_block(path[level], node.bytes());
            status != ours::Status::Ok) {
            return status;
        }
        if (auto const status = device.write_block(right.unwrap(), right_node.bytes());
            status != ours::Status::Ok) {
            return status;
        }
        up_key = mid;
        up_child = right.unwrap();
    }

    auto const new_root = alloc_block(device, volume);
    if (new_root.is_err()) {
        return new_root.unwrap_err();
    }
    auto root_bytes = std::array<u8, BLOCK_BYTES>{};
    auto root = detail::Node::internal(root_bytes);
    root.insert_child(0, up_key, up_child);
    // The new root's first child is the old root, the pair's left side.
    root.set_child(0, volume.root);
    if (auto const status = device.write_block(new_root.unwrap(), root.bytes());
        status != ours::Status::Ok) {
        return status;
    }
    volume.root = new_root.unwrap();
    volume.height = static_cast<u8>(volume.height + 1);
    return persist_super(device, volume);
}

namespace detail {
    /// Fixes the underflowed child |key| routes to of the internal node |node|:
    /// a borrow from a sibling that can spare, otherwise a merge whose routing
    /// pair leaves the node. |underflow| answers whether the node itself now
    /// carries fewer than its minimum.
    template <typename Device>
    auto fix_child(Device &device, Volume &volume, Node &node, usize const index, bool &underflow)
        -> ours::Status {
        auto cur_bytes = std::array<u8, BLOCK_BYTES>{};
        if (auto const status = device.read_block(node.child(index), cur_bytes);
            status != ours::Status::Ok) {
            return status;
        }
        auto cur = Node{cur_bytes};
        // The minimum that matters is the child's own: a leaf's 25, an
        // internal node's 84, by what the child is and not by what its
        // parent is.
        auto const min = cur.minimum();

        // The left sibling first: it lends its last, and the separator between
        // it and the child becomes what the child now starts with.
        auto left_bytes = std::array<u8, BLOCK_BYTES>{};
        if (index > 0) {
            if (auto const status = device.read_block(node.child(index - 1), left_bytes);
                status != ours::Status::Ok) {
                return status;
            }
            auto left = Node{left_bytes};
            if (left.count() > min) {
                auto const separator =
                    cur.is_leaf() ? cur.borrow_last_from(left)
                                  : cur.borrow_last_from_internal(left, node.key(index - 1));
                node.set_key(index - 1, separator);
                if (auto const written = device.write_block(node.child(index - 1), left.bytes());
                    written != ours::Status::Ok) {
                    return written;
                }
                if (auto const written = device.write_block(node.child(index), cur.bytes());
                    written != ours::Status::Ok) {
                    return written;
                }
                underflow = node.count() < node.minimum();
                return ours::Status::Ok;
            }
        }

        // The right sibling next, the same the other way.
        auto right_bytes = std::array<u8, BLOCK_BYTES>{};
        if (index < node.count()) {
            if (auto const status = device.read_block(node.child(index + 1), right_bytes);
                status != ours::Status::Ok) {
                return status;
            }
            auto right = Node{right_bytes};
            if (right.count() > min) {
                auto const separator = cur.is_leaf()
                                           ? cur.borrow_first_from(right)
                                           : cur.borrow_first_from_internal(right, node.key(index));
                node.set_key(index, separator);
                if (auto const written = device.write_block(node.child(index + 1), right.bytes());
                    written != ours::Status::Ok) {
                    return written;
                }
                if (auto const written = device.write_block(node.child(index), cur.bytes());
                    written != ours::Status::Ok) {
                    return written;
                }
                underflow = node.count() < node.minimum();
                return ours::Status::Ok;
            }
        }

        // No sibling can spare: the merge. The left one is preferred, so the
        // child slides into what precedes it and dies; with no left, the right
        // slides into the child.
        if (index > 0) {
            auto left = Node{left_bytes};
            if (cur.is_leaf()) {
                left.absorb_leaf(cur);
            } else {
                left.absorb_internal(cur, node.key(index - 1));
            }
            if (auto const written = device.write_block(node.child(index - 1), left.bytes());
                written != ours::Status::Ok) {
                return written;
            }
            if (auto const status = free_block(device, volume, node.child(index));
                status != ours::Status::Ok) {
                return status;
            }
            node.erase_child(index - 1);
        } else {
            auto right = Node{right_bytes};
            if (cur.is_leaf()) {
                cur.absorb_leaf(right);
            } else {
                cur.absorb_internal(right, node.key(0));
            }
            if (auto const written = device.write_block(node.child(0), cur.bytes());
                written != ours::Status::Ok) {
                return written;
            }
            if (auto const status = free_block(device, volume, node.child(1));
                status != ours::Status::Ok) {
                return status;
            }
            node.erase_child(0);
        }
        underflow = node.count() < node.minimum();
        return ours::Status::Ok;
    }

    /// Removes the key from the subtree |block| roots at |height| levels,
    /// answering |underflow| -- whether the node now carries fewer than its
    /// minimum and its parent must rebalance. The parent's turn happens one
    /// frame up, which is what makes the cascade: a merge there removes a
    /// routing pair, and the same rule applies again.
    template <typename Device>
    auto remove_from(Device &device, Volume &volume, u64 block, u8 height, Key const &key,
                     bool &underflow) -> ours::Status {
        auto bytes = std::array<u8, BLOCK_BYTES>{};
        if (auto const status = device.read_block(block, bytes); status != ours::Status::Ok) {
            return status;
        }
        auto node = Node{bytes};
        if (height == 1) {
            auto const at = node.upper_bound(key);
            if (at == 0 || node.key(at - 1) != key) {
                return ours::Status::NotFound;
            }
            node.erase_leaf(at - 1);
            underflow = node.count() < node.minimum();
            return device.write_block(block, node.bytes());
        }
        auto const index = node.upper_bound(key);
        auto child_under = false;
        if (auto const status = remove_from(device, volume, node.child(index),
                                            static_cast<u8>(height - 1), key, child_under);
            status != ours::Status::Ok) {
            return status;
        }
        if (!child_under) {
            underflow = false;
            return ours::Status::Ok;
        }
        if (auto const status = fix_child(device, volume, node, index, underflow);
            status != ours::Status::Ok) {
            return status;
        }
        return device.write_block(block, node.bytes());
    }
} // namespace detail

/// Removes the item |key| names, rebalancing what the removal leaves past
/// its minimum: a leaf borrows from a sibling, or merges with one -- the
/// merge taking the routing pair out of the parent, which may underflow in
/// its turn -- and a root left with one child is replaced by that child,
/// the tree a level shorter. The blocks of merged-away nodes go back to the
/// volume's bitmap. NotFound when no item carries the key.
template <typename Device>
auto remove(Device &device, Volume &volume, Key const &key) -> ours::Status {
    if (volume.height == 0) {
        return ours::Status::NotFound;
    }
    auto underflow = false;
    if (auto const status =
            detail::remove_from(device, volume, volume.root, volume.height, key, underflow);
        status != ours::Status::Ok) {
        return status;
    }
    // The root never borrows -- it carries what the tree needs -- but an
    // internal root left with a single child is a level for nothing: the
    // child rises, the old root's block goes back.
    while (volume.height > 1) {
        auto bytes = std::array<u8, BLOCK_BYTES>{};
        if (auto const status = device.read_block(volume.root, bytes); status != ours::Status::Ok) {
            return status;
        }
        auto node = detail::Node{bytes};
        if (node.count() != 0) {
            break;
        }
        auto const child = node.child(0);
        if (auto const status = free_block(device, volume, volume.root);
            status != ours::Status::Ok) {
            return status;
        }
        volume.root = child;
        volume.height = static_cast<u8>(volume.height - 1);
        if (auto const status = persist_super(device, volume); status != ours::Status::Ok) {
            return status;
        }
    }
    return ours::Status::Ok;
}

/// Calls |visit| on every item from |from| onward, in key order, along the
/// leaves' chain, until |visit| answers false or the chain ends. |visit|
/// takes (Key const &, Value const &) and answers bool.
template <typename Device, typename Visit>
auto scan(Device &device, Volume const &volume, Key const &from, Visit &&visit) -> ours::Status {
    if (volume.height == 0) {
        return ours::Status::Ok;
    }
    // Down to the leaf that would hold |from|, then right along the chain.
    auto block = volume.root;
    auto bytes = std::array<u8, BLOCK_BYTES>{};
    for (u8 level = 0; level + 1 < volume.height; ++level) {
        if (auto const status = device.read_block(block, bytes); status != ours::Status::Ok) {
            return status;
        }
        block = detail::Node{bytes}.child_for(from);
    }
    while (block != 0) {
        if (auto const status = device.read_block(block, bytes); status != ours::Status::Ok) {
            return status;
        }
        auto node = detail::Node{bytes};
        for (auto slot = usize{0}; slot < node.count(); ++slot) {
            auto const key = node.key(slot);
            if (key < from) {
                continue;
            }
            if (!visit(key, node.value(slot))) {
                return ours::Status::Ok;
            }
        }
        block = node.next_leaf();
    }
    return ours::Status::Ok;
}
} // namespace ours_btreefs

#endif // #ifndef OURS_BTREEFS_OURS_BTREEFS_HPP
