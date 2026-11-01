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
///     block, the tree's height, and where the free bitmap lives;
///   - the free bitmap, contiguous blocks from block 1, one bit per block;
///   - B+tree nodes, one block each: a header (leaf flag, entry count,
///     next-leaf pointer) and slots. An internal node keeps child pointer
///     and key together per slot -- child i covers the keys from key i-1 up
///     to but excluding key i; a leaf keeps key and value per slot and
///     chains to the next leaf in key order.
///
/// The one tree answers everything. A key names (object, type, offset):
/// an object's INODE item (offset 0) carries its size and kind, a
/// directory's DIRENT items (offset = the name's hash) its children, a
/// file's DATA items (offset = the block's index in the file) its blocks.
/// Lookup, insert and split are the same code for all of them; only the
/// type byte and the value's meaning differ.
///
/// The events of an insert: descend from the root, at each internal node
/// taking the child whose range covers the key; at the leaf, make room and
/// slot the item in -- or, the leaf full, split it into a right sibling
/// whose first key becomes the separator pushed into the parent, the parent
/// splitting the same way, a root's split growing the tree one level. The
/// separator is a routing copy: the item itself stays in a leaf, and a
/// lookup for the separator's key must land in the right sibling, which is
/// why the child an internal node routes to is the one past the last key
/// that is not greater than the target.
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

/// One stored item, a key and the value that belongs to it.
struct Item {
    Key key;
    Value value = {};
};

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
/// is a leaf and 0 a volume with no tree yet.
struct Volume {
    u64 blocks = 0;
    u64 root = 0;
    u8 height = 0;
    u64 bitmap_first = 0;
    u32 bitmap_blocks = 0;
};

/// The hash a DIRENT's offset carries: FNV-1a over the name's bytes.
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

namespace detail {
    /// A node's layout, from its first byte: the leaf flag at 0, the entry
    /// count at 2, the next-leaf pointer at 8, the slots from 16.
    constexpr auto LEAF_FLAG_OFFSET = usize{0};
    constexpr auto COUNT_OFFSET = usize{2};
    constexpr auto NEXT_LEAF_OFFSET = usize{8};
    constexpr auto SLOTS_OFFSET = usize{16};

    constexpr auto LEAF_FLAG = u8{1};

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

    [[nodiscard]]
    auto load_le16(std::span<u8 const> bytes, usize at) -> u16;

    [[nodiscard]]
    auto load_le32(std::span<u8 const> bytes, usize at) -> u32;

    [[nodiscard]]
    auto load_le64(std::span<u8 const> bytes, usize at) -> u64;

    auto store_le16(std::span<u8> bytes, usize at, u16 value) -> void;

    auto store_le32(std::span<u8> bytes, usize at, u32 value) -> void;

    auto store_le64(std::span<u8> bytes, usize at, u64 value) -> void;

    /// A leaf's slot |slot| occupies [leaf_slot_offset(slot),
    /// leaf_slot_offset(slot) + KEY_BYTES + VALUE_BYTES): key first, value
    /// after it. An internal node's slot is child first, key after.
    [[nodiscard]]
    constexpr auto leaf_slot_offset(usize slot) -> usize {
        return SLOTS_OFFSET + slot * (KEY_BYTES + VALUE_BYTES);
    }

    [[nodiscard]]
    constexpr auto internal_slot_offset(usize slot) -> usize {
        return SLOTS_OFFSET + slot * (KEY_BYTES + 8);
    }

    [[nodiscard]]
    auto leaf_key(std::span<u8 const> node, usize slot) -> Key;

    [[nodiscard]]
    auto internal_key(std::span<u8 const> node, usize slot) -> Key;

    auto store_key(std::span<u8> node, usize at, Key const &key) -> void;

    /// The leading keys of |node| that are not greater than |key|: the count
    /// upper_bound answers. An internal node routes to the child at that
    /// index, so the separator's own key routes right, where its item lives.
    [[nodiscard]]
    auto leaf_upper_bound(std::span<u8 const> node, usize count, Key const &key) -> usize;

    [[nodiscard]]
    auto internal_upper_bound(std::span<u8 const> node, usize count, Key const &key) -> usize;

    /// The child an internal node routes |key| to.
    [[nodiscard]]
    auto child_for(std::span<u8 const> node, usize count, Key const &key) -> u64;

    [[nodiscard]]
    auto leaf_value(std::span<u8 const> node, usize slot) -> Value;

    auto store_leaf_value(std::span<u8> node, usize slot, Value const &value) -> void;

    [[nodiscard]]
    auto internal_child(std::span<u8 const> node, usize slot) -> u64;

    auto store_internal_child(std::span<u8> node, usize slot, u64 child) -> void;

    /// Shifts a leaf's slots from |at| one right and puts |key| with |value|
    /// into the gap; |count| is the count before and answers after.
    auto leaf_insert(std::span<u8> node, usize &count, usize at, Key const &key, Value const &value)
        -> void;

    /// Shifts an internal node's keys and children to open the routing pair
    /// |key| -> |child| between child |at| and child |at| + 1; |count| is the
    /// key count before and answers after.
    auto internal_insert(std::span<u8> node, usize &count, usize at, Key const &key, u64 child)
        -> void;

    /// Splits the full leaf |left|: the upper half of its items moves into the
    /// fresh |right|, which takes over |left|'s chain link, and the separator
    /// -- right's first key, a copy that only routes -- is answered.
    [[nodiscard]]
    auto split_leaf(std::span<u8> left, std::span<u8> right) -> Key;

    /// Splits the full internal node |left| into |right|, answering the key
    /// that climbs: it stays in neither node.
    [[nodiscard]]
    auto split_internal(std::span<u8> left, std::span<u8> right) -> Key;

    /// Writes a fresh root over |left| and |right| split at |key|, and answers
    /// the block the caller has just written it to -- the caller allocates.
    auto fill_root(std::span<u8> node, u64 left, Key const &key, u64 right) -> void;
} // namespace detail

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
    detail::store_le64(super, 8, blocks);
    detail::store_le64(super, 16, 0);
    detail::store_le64(super, 32, 1);
    detail::store_le32(super, 40, bitmap_blocks);
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
/// change root or height call it; the caller persists nothing.
template <typename Device>
auto persist_super(Device &device, Volume const &volume) -> ours::Status {
    auto super = std::array<u8, BLOCK_BYTES>{};
    if (auto const status = device.read_block(0, std::span{super}); status != ours::Status::Ok) {
        return status;
    }
    detail::store_le64(super, 16, volume.root);
    super[24] = volume.height;
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
    if (std::string_view{reinterpret_cast<char const *>(super.data()), MAGIC.size()} != MAGIC) {
        return ustl::err(ours::Status::NotFound);
    }
    auto volume = Volume{};
    volume.blocks = detail::load_le64(super, 8);
    volume.root = detail::load_le64(super, 16);
    volume.height = super[24];
    volume.bitmap_first = detail::load_le64(super, 32);
    volume.bitmap_blocks = detail::load_le32(super, 40);
    if (volume.root != 0 && volume.height == 0) {
        return ustl::err(ours::Status::BadState);
    }
    return ustl::ok(volume);
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
    if (block < volume.bitmap_first + u64{volume.bitmap_blocks}) {
        return ours::Status::InvArgs;
    }
    if (block >= volume.blocks || index >= u64{volume.bitmap_blocks} * BLOCK_BYTES) {
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
    auto block = volume.root;
    for (u8 level = 0; level < volume.height; ++level) {
        auto node = std::array<u8, BLOCK_BYTES>{};
        if (auto const status = device.read_block(block, std::span{node});
            status != ours::Status::Ok) {
            return status;
        }
        auto const count = usize{detail::load_le16(node, detail::COUNT_OFFSET)};
        if (level + 1 == volume.height) {
            auto const at = detail::leaf_upper_bound(node, count, key);
            if (at == 0 || detail::leaf_key(node, at - 1) != key) {
                return ours::Status::NotFound;
            }
            value = detail::leaf_value(node, at - 1);
            return ours::Status::Ok;
        }
        block = detail::child_for(node, count, key);
    }
    return ours::Status::NotFound;
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
    for (u8 level = 0; level + 1 < volume.height; ++level) {
        auto node = std::array<u8, BLOCK_BYTES>{};
        if (auto const status = device.read_block(block, std::span{node});
            status != ours::Status::Ok) {
            return status;
        }
        block = detail::child_for(node, detail::load_le16(node, detail::COUNT_OFFSET), from);
    }
    while (block != 0) {
        auto node = std::array<u8, BLOCK_BYTES>{};
        if (auto const status = device.read_block(block, std::span{node});
            status != ours::Status::Ok) {
            return status;
        }
        auto const count = usize{detail::load_le16(node, detail::COUNT_OFFSET)};
        for (auto slot = usize{0}; slot < count; ++slot) {
            auto const key = detail::leaf_key(node, slot);
            if (key < from) {
                continue;
            }
            if (!visit(key, detail::leaf_value(node, slot))) {
                return ours::Status::Ok;
            }
        }
        block = detail::load_le64(node, detail::NEXT_LEAF_OFFSET);
    }
    return ours::Status::Ok;
}

namespace detail {
    /// Which of the pair |left|/|right| takes |key| after |left| split at
    /// |separator|: the half whose range covers it.
    [[nodiscard]]
    inline auto half_for(std::span<u8> left, std::span<u8> right, Key const &key,
                         Key const &separator) -> std::span<u8> {
        if (key < separator) {
            return left;
        }
        return right;
    }
} // namespace detail

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
        auto node = std::array<u8, BLOCK_BYTES>{};
        node[detail::LEAF_FLAG_OFFSET] = detail::LEAF_FLAG;
        if (auto const status = device.write_block(leaf.unwrap(), std::span{node});
            status != ours::Status::Ok) {
            return status;
        }
        volume.root = leaf.unwrap();
        volume.height = 1;
        if (auto const status = persist_super(device, volume); status != ours::Status::Ok) {
            return status;
        }
    }

    // The path down, each level's block and buffer kept for the walk back up
    // that a split's separator push is.
    auto path = std::array<std::pair<u64, std::array<u8, BLOCK_BYTES>>, 16>{};
    if (u64{volume.height} > path.size()) {
        return ours::Status::BadState;
    }
    path[0].first = volume.root;
    for (u8 level = 0; level + 1 < volume.height; ++level) {
        auto &node = path[level].second;
        if (auto const status = device.read_block(path[level].first, std::span{node});
            status != ours::Status::Ok) {
            return status;
        }
        path[level + 1].first =
            detail::child_for(node, detail::load_le16(node, detail::COUNT_OFFSET), key);
    }
    auto &leaf_node = path[volume.height - 1].second;
    if (auto const status = device.read_block(path[volume.height - 1].first, std::span{leaf_node});
        status != ours::Status::Ok) {
        return status;
    }
    auto leaf_count = usize{detail::load_le16(leaf_node, detail::COUNT_OFFSET)};
    auto const at = detail::leaf_upper_bound(leaf_node, leaf_count, key);
    if (at > 0 && detail::leaf_key(leaf_node, at - 1) == key) {
        return ours::Status::AlreadyExists;
    }

    if (leaf_count < LEAF_CAPACITY) {
        detail::leaf_insert(leaf_node, leaf_count, at, key, value);
        return device.write_block(path[volume.height - 1].first, std::span{leaf_node});
    }

    // The leaf is full: split it, put the item into whichever half its key
    // sorts into, and send the separator up.
    auto right_leaf = std::array<u8, BLOCK_BYTES>{};
    right_leaf[detail::LEAF_FLAG_OFFSET] = detail::LEAF_FLAG;
    auto const separator = detail::split_leaf(leaf_node, right_leaf);
    auto target = detail::half_for(leaf_node, right_leaf, key, separator);
    auto target_count = usize{detail::load_le16(target, detail::COUNT_OFFSET)};
    auto const target_at = detail::leaf_upper_bound(target, target_count, key);
    detail::leaf_insert(target, target_count, target_at, key, value);
    auto const right_block = alloc_block(device, volume);
    if (right_block.is_err()) {
        return right_block.unwrap_err();
    }
    // The chain: the right leaf took |left|'s old link in the split; the
    // left one now points at the right.
    detail::store_le64(leaf_node, detail::NEXT_LEAF_OFFSET, right_block.unwrap());
    if (auto const status = device.write_block(path[volume.height - 1].first, std::span{leaf_node});
        status != ours::Status::Ok) {
        return status;
    }
    if (auto const status = device.write_block(right_block.unwrap(), std::span{right_leaf});
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
        auto &node = path[level].second;
        auto count = usize{detail::load_le16(node, detail::COUNT_OFFSET)};
        auto const at = detail::internal_upper_bound(node, count, up_key);
        if (count < INTERNAL_CAPACITY) {
            detail::internal_insert(node, count, at, up_key, up_child);
            return device.write_block(path[level].first, std::span{node});
        }
        auto right_node = std::array<u8, BLOCK_BYTES>{};
        auto const mid = detail::split_internal(node, right_node);
        auto target = detail::half_for(node, right_node, up_key, mid);
        auto target_count = usize{detail::load_le16(target, detail::COUNT_OFFSET)};
        auto const target_at = detail::internal_upper_bound(target, target_count, up_key);
        detail::internal_insert(target, target_count, target_at, up_key, up_child);
        auto const right = alloc_block(device, volume);
        if (right.is_err()) {
            return right.unwrap_err();
        }
        if (auto const status = device.write_block(path[level].first, std::span{node});
            status != ours::Status::Ok) {
            return status;
        }
        if (auto const status = device.write_block(right.unwrap(), std::span{right_node});
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
    auto root_node = std::array<u8, BLOCK_BYTES>{};
    detail::fill_root(root_node, volume.root, up_key, up_child);
    if (auto const status = device.write_block(new_root.unwrap(), std::span{root_node});
        status != ours::Status::Ok) {
        return status;
    }
    volume.root = new_root.unwrap();
    volume.height = static_cast<u8>(volume.height + 1);
    return persist_super(device, volume);
}

namespace detail {
    /// A leaf's insert of the routing pair's key, with the count bookkeeping
    /// the caller's |count| holds.
    inline auto leaf_insert(std::span<u8> node, usize &count, usize at, Key const &key,
                            Value const &value) -> void {
        auto const slot = leaf_slot_offset(at);
        auto const tail = (count - at) * (KEY_BYTES + VALUE_BYTES);
        std::memmove(node.data() + slot + KEY_BYTES + VALUE_BYTES, node.data() + slot, tail);
        store_key(node, slot, key);
        store_leaf_value(node, at, value);
        count += 1;
        store_le16(node, COUNT_OFFSET, static_cast<u16>(count));
    }

    inline auto internal_insert(std::span<u8> node, usize &count, usize at, Key const &key,
                                u64 child) -> void {
        // The keys from |at| move one slot right; the children from |at| + 1
        // with them, the new child landing after the child that covered the
        // range the new key now splits.
        auto const from = internal_slot_offset(at) + 8;
        auto const tail = (count - at) * (KEY_BYTES + 8);
        std::memmove(node.data() + from + KEY_BYTES + 8, node.data() + from, tail);
        store_key(node, from, key);
        store_internal_child(node, at + 1, child);
        count += 1;
        store_le16(node, COUNT_OFFSET, static_cast<u16>(count));
    }
} // namespace detail
} // namespace ours_btreefs

#endif // #ifndef OURS_BTREEFS_OURS_BTREEFS_HPP
