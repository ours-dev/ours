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

    auto leaf_erase(std::span<u8> node, usize &count, usize at) -> void {
        auto const slot = leaf_slot_offset(at);
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;
        auto const tail = (count - at - 1) * slot_bytes;
        std::memmove(node.data() + slot, node.data() + slot + slot_bytes, tail);
        count -= 1;
        store_le16(node, COUNT_OFFSET, static_cast<u16>(count));
    }

    auto leaf_borrow_left(std::span<u8> dst, usize &dst_count, std::span<u8> left,
                          usize &left_count) -> Key {
        // The left sibling's last item moves to the front: the destination's
        // slots shift right one to open the slot, and the moved key is what the
        // parent now routes the pair by.
        dst_count += 1;
        store_le16(dst, COUNT_OFFSET, static_cast<u16>(dst_count));
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;
        std::memmove(dst.data() + leaf_slot_offset(1), dst.data() + leaf_slot_offset(0),
                     (dst_count - 1) * slot_bytes);
        auto const from = leaf_slot_offset(left_count - 1);
        std::memcpy(dst.data() + leaf_slot_offset(0), left.data() + from, slot_bytes);
        auto const moved = leaf_key(left, left_count - 1);
        left_count -= 1;
        store_le16(left, COUNT_OFFSET, static_cast<u16>(left_count));
        return moved;
    }

    auto leaf_borrow_right(std::span<u8> dst, usize &dst_count, std::span<u8> right,
                           usize &right_count) -> Key {
        // The right sibling's first item moves to the back, and the right's new
        // first key is what the parent routes the pair by.
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;
        std::memcpy(dst.data() + leaf_slot_offset(dst_count), right.data() + leaf_slot_offset(0),
                    slot_bytes);
        dst_count += 1;
        store_le16(dst, COUNT_OFFSET, static_cast<u16>(dst_count));
        std::memmove(right.data() + leaf_slot_offset(0), right.data() + leaf_slot_offset(1),
                     (right_count - 1) * slot_bytes);
        right_count -= 1;
        store_le16(right, COUNT_OFFSET, static_cast<u16>(right_count));
        return leaf_key(right, 0);
    }

    auto leaf_merge(std::span<u8> dst, usize &dst_count, std::span<u8> right, usize &right_count)
        -> void {
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;
        std::memcpy(dst.data() + leaf_slot_offset(dst_count), right.data() + leaf_slot_offset(0),
                    right_count * slot_bytes);
        dst_count += right_count;
        store_le16(dst, COUNT_OFFSET, static_cast<u16>(dst_count));
        // The chain: what followed |right| follows |dst| now.
        store_le64(dst, NEXT_LEAF_OFFSET, load_le64(right, NEXT_LEAF_OFFSET));
        right_count = 0;
        store_le16(right, COUNT_OFFSET, 0);
    }

    auto internal_borrow_left(std::span<u8> dst, usize &dst_count, std::span<u8> left,
                              usize &left_count, Key const &separator) -> Key {
        // The left sibling's last child and last key move over: the child in
        // front, the parent's separator as the destination's first key, and the
        // left's old last key up to the parent.
        dst_count += 1;
        store_le16(dst, COUNT_OFFSET, static_cast<u16>(dst_count));
        auto const slot_bytes = KEY_BYTES + 8;
        std::memmove(dst.data() + internal_slot_offset(1), dst.data() + internal_slot_offset(0),
                     (dst_count - 1) * slot_bytes + 8);
        store_internal_child(dst, 0, internal_child(left, left_count));
        store_key(dst, internal_slot_offset(0) + 8, separator);
        auto const moved = internal_key(left, left_count - 1);
        left_count -= 1;
        store_le16(left, COUNT_OFFSET, static_cast<u16>(left_count));
        return moved;
    }

    auto internal_borrow_right(std::span<u8> dst, usize &dst_count, std::span<u8> right,
                               usize &right_count, Key const &separator) -> Key {
        // The right sibling's first child and first key move over: the parent's
        // separator as the destination's last key, the right's old first key up
        // to the parent, and the right's second child becomes its first.
        store_internal_child(dst, dst_count + 1, internal_child(right, 0));
        store_key(dst, internal_slot_offset(dst_count) + 8, separator);
        dst_count += 1;
        store_le16(dst, COUNT_OFFSET, static_cast<u16>(dst_count));
        auto const moved = internal_key(right, 0);
        auto const slot_bytes = KEY_BYTES + 8;
        std::memmove(right.data() + internal_slot_offset(0), right.data() + internal_slot_offset(1),
                     (right_count - 1) * slot_bytes + 8);
        right_count -= 1;
        store_le16(right, COUNT_OFFSET, static_cast<u16>(right_count));
        return moved;
    }

    auto internal_merge(std::span<u8> dst, usize &dst_count, std::span<u8> right,
                        usize &right_count, Key const &separator) -> void {
        // The parent's separator descends as the pair's key, and the
        // right's slots follow whole -- slot for slot, the right's first
        // child among them, its last child as the destination's new last.
        store_key(dst, internal_slot_offset(dst_count) + 8, separator);
        auto const slot_bytes = KEY_BYTES + 8;
        std::memcpy(dst.data() + internal_slot_offset(dst_count + 1),
                    right.data() + internal_slot_offset(0), right_count * slot_bytes);
        store_internal_child(dst, dst_count + 1 + right_count, internal_child(right, right_count));
        dst_count += right_count + 1;
        store_le16(dst, COUNT_OFFSET, static_cast<u16>(dst_count));
        right_count = 0;
        store_le16(right, COUNT_OFFSET, 0);
    }

    auto internal_erase(std::span<u8> node, usize &count, usize at) -> void {
        // The key at |at| and the child at |at| + 1 go: everything from the
        // key's position shifts one slot left, the last child with it.
        auto const slot_bytes = KEY_BYTES + 8;
        auto const from = internal_slot_offset(at) + 8;
        // From the key through the last child: the child at |at| + 1 dies
        // with the key, the ones after it and the node's last child shift
        // one slot.
        auto const tail = (count - at) * slot_bytes;
        std::memmove(node.data() + from, node.data() + from + slot_bytes, tail);
        count -= 1;
        store_le16(node, COUNT_OFFSET, static_cast<u16>(count));
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
