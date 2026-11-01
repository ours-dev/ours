/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The format's byte level: little-endian loads and stores, the key's disk
// form, the three values' codecs, the nodes' upper bounds, and the splits.
// The operations themselves are templates over the block device and live in
// the header; everything here is device-free.
#include <cstring>

#include <ours_btreefs/ours_btreefs.hpp>

namespace ours_btreefs {
namespace detail {
    auto load_le16(std::span<u8 const> bytes, usize at) -> u16 {
        return static_cast<u16>(bytes[at]) | static_cast<u16>(static_cast<u16>(bytes[at + 1]) << 8);
    }

    auto load_le32(std::span<u8 const> bytes, usize at) -> u32 {
        return static_cast<u32>(load_le16(bytes, at)) |
               static_cast<u32>(static_cast<u32>(load_le16(bytes, at + 2)) << 16);
    }

    auto load_le64(std::span<u8 const> bytes, usize at) -> u64 {
        return u64{load_le32(bytes, at)} | (u64{load_le32(bytes, at + 4)} << 32);
    }

    auto store_le16(std::span<u8> bytes, usize at, u16 value) -> void {
        bytes[at] = static_cast<u8>(value);
        bytes[at + 1] = static_cast<u8>(value >> 8);
    }

    auto store_le32(std::span<u8> bytes, usize at, u32 value) -> void {
        store_le16(bytes, at, static_cast<u16>(value));
        store_le16(bytes, at + 2, static_cast<u16>(value >> 16));
    }

    auto store_le64(std::span<u8> bytes, usize at, u64 value) -> void {
        store_le32(bytes, at, static_cast<u32>(value));
        store_le32(bytes, at + 4, static_cast<u32>(value >> 32));
    }

    auto load_key(std::span<u8 const> node, usize at) -> Key {
        auto const word = load_le64(node, at + 8);
        return Key{.object = load_le64(node, at),
                   .type = unpack_type(word),
                   .offset = unpack_offset(word)};
    }

    auto store_key(std::span<u8> node, usize at, Key const &key) -> void {
        store_le64(node, at, key.object);
        store_le64(node, at + 8, pack_type_offset(key.type, key.offset));
    }

    namespace {
        /// The binary search both nodes' upper bounds are: how many leading keys
        /// are not greater than |key|, |key_at| answering the key at a slot.
        template <typename KeyAt>
        auto upper_bound(usize count, Key const &key, KeyAt &&key_at) -> usize {
            auto low = usize{0};
            auto high = count;
            while (low < high) {
                auto const mid = low + (high - low) / 2;
                if (key_at(mid) <= key) {
                    low = mid + 1;
                } else {
                    high = mid;
                }
            }
            return low;
        }
    } // namespace

    auto leaf_upper_bound(std::span<u8 const> node, usize count, Key const &key) -> usize {
        return upper_bound(count, key, [&](usize slot) { return leaf_key(node, slot); });
    }

    auto internal_upper_bound(std::span<u8 const> node, usize count, Key const &key) -> usize {
        return upper_bound(count, key, [&](usize slot) { return internal_key(node, slot); });
    }

    auto child_for(std::span<u8 const> node, usize count, Key const &key) -> u64 {
        return internal_child(node, internal_upper_bound(node, count, key));
    }

    auto leaf_key(std::span<u8 const> node, usize slot) -> Key {
        return load_key(node, leaf_slot_offset(slot));
    }

    auto internal_key(std::span<u8 const> node, usize slot) -> Key {
        return load_key(node, internal_slot_offset(slot) + 8);
    }

    auto leaf_value(std::span<u8 const> node, usize slot) -> Value {
        auto value = Value{};
        std::memcpy(value.data(), node.data() + leaf_slot_offset(slot) + KEY_BYTES, VALUE_BYTES);
        return value;
    }

    auto store_leaf_value(std::span<u8> node, usize slot, Value const &value) -> void {
        std::memcpy(node.data() + leaf_slot_offset(slot) + KEY_BYTES, value.data(), VALUE_BYTES);
    }

    auto internal_child(std::span<u8 const> node, usize slot) -> u64 {
        return load_le64(node, internal_slot_offset(slot));
    }

    auto store_internal_child(std::span<u8> node, usize slot, u64 child) -> void {
        store_le64(node, internal_slot_offset(slot), child);
    }

    auto split_leaf(std::span<u8> left, std::span<u8> right) -> Key {
        auto const count = usize{load_le16(left, COUNT_OFFSET)};
        auto const keep = count / 2;
        auto const moved = count - keep;
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;

        // The upper half of the items moves whole, then the counts: the right
        // leaf takes over the chain link first, the caller re-links |left| to
        // the right leaf's block once it has allocated one.
        std::memcpy(right.data() + SLOTS_OFFSET, left.data() + leaf_slot_offset(keep),
                    moved * slot_bytes);
        store_le64(right, NEXT_LEAF_OFFSET, load_le64(left, NEXT_LEAF_OFFSET));
        store_le16(right, COUNT_OFFSET, static_cast<u16>(moved));
        store_le16(left, COUNT_OFFSET, static_cast<u16>(keep));
        return leaf_key(right, 0);
    }

    auto split_internal(std::span<u8> left, std::span<u8> right) -> Key {
        auto const count = usize{load_le16(left, COUNT_OFFSET)};
        auto const mid = count / 2;
        auto const right_count = count - mid - 1;

        // Right's slot j takes the old child mid+1+j and the old key mid+1+j,
        // with the old child count+1 as its one child more than its keys; the
        // key at |mid| is answered and stays in neither node.
        for (auto slot = usize{0}; slot <= right_count; ++slot) {
            store_internal_child(right, slot, internal_child(left, mid + 1 + slot));
            if (slot < right_count) {
                store_key(right, internal_slot_offset(slot) + 8,
                          internal_key(left, mid + 1 + slot));
            }
        }
        store_le16(right, COUNT_OFFSET, static_cast<u16>(right_count));
        store_le16(left, COUNT_OFFSET, static_cast<u16>(mid));
        return internal_key(left, mid);
    }

    auto fill_root(std::span<u8> node, u64 left, Key const &key, u64 right) -> void {
        store_internal_child(node, 0, left);
        store_key(node, internal_slot_offset(0) + 8, key);
        store_internal_child(node, 1, right);
        store_le16(node, COUNT_OFFSET, 1);
    }
} // namespace detail

auto dirent_offset(std::string_view name) -> u64 {
    // FNV-1a, 64 bits: the offset basis, then prime and XOR per byte. The
    // offset a DIRENT carries is the key's 56 bits wide, so the hash's top
    // 8 bits fold into the bottom -- no name refused for being hashed high,
    // and no two of the folded values lost.
    auto constexpr BASIS = u64{14695981039346656037ULL};
    auto constexpr PRIME = u64{1099511628211ULL};
    auto hash = BASIS;
    for (auto const byte: name) {
        hash ^= byte;
        hash *= PRIME;
    }
    return (hash >> detail::OFFSET_BITS) ^ (hash & detail::OFFSET_MASK);
}

auto encode_inode_value(Value &out, u64 size, ObjectKind kind) -> void {
    detail::store_le64(out, 0, size);
    out[8] = std::to_underlying(kind);
}

auto decode_inode_value(Value const &value) -> ustl::Result<InodeValue, ours::Status> {
    auto const kind = static_cast<ObjectKind>(value[8]);
    if (kind != ObjectKind::File && kind != ObjectKind::Directory) {
        return ustl::err(ours::Status::InvArgs);
    }
    return ustl::ok(InodeValue{.size = detail::load_le64(value, 0), .kind = kind});
}

auto encode_dirent_value(Value &out, u64 child, ObjectKind kind, std::string_view name)
    -> ours::Status {
    if (name.size() > MAX_NAME_BYTES) {
        return ours::Status::InvArgs;
    }
    detail::store_le64(out, 0, child);
    out[8] = std::to_underlying(kind);
    out[9] = static_cast<u8>(name.size());
    std::memcpy(out.data() + 10, name.data(), name.size());
    return ours::Status::Ok;
}

auto decode_dirent_value(Value const &value) -> ustl::Result<DirentValue, ours::Status> {
    auto const kind = static_cast<ObjectKind>(value[8]);
    auto const length = usize{value[9]};
    if ((kind != ObjectKind::File && kind != ObjectKind::Directory) || length > MAX_NAME_BYTES) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto const name = std::string_view{reinterpret_cast<char const *>(value.data()) + 10, length};
    return ustl::ok(DirentValue{.child = detail::load_le64(value, 0), .kind = kind, .name = name});
}

auto encode_data_value(Value &out, u64 block) -> void {
    detail::store_le64(out, 0, block);
}

auto decode_data_value(Value const &value) -> u64 {
    return detail::load_le64(value, 0);
}
} // namespace ours_btreefs
