/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The format's byte level: the little-endian loads and stores, the node's
// layout and its slot surgery, the superblock's fields, and the three
// values' codecs. The operations are templates over the block device and
// live in the header, where they ask the node and never touch an offset.
#include <cstring>

#include <ours_btreefs/ours_btreefs.hpp>
#include <ours_gpt/ours_gpt.hpp>

namespace ours_btreefs {
namespace {
    /// The three values' layouts: an INODE's size and kind, a DIRENT's child,
    /// kind, length and name, a DATA's block.
    constexpr auto INODE_SIZE_OFFSET = usize{0};
    constexpr auto INODE_KIND_OFFSET = usize{8};
    constexpr auto DIRENT_CHILD_OFFSET = usize{0};
    constexpr auto DIRENT_KIND_OFFSET = usize{8};
    constexpr auto DIRENT_NAME_LENGTH_OFFSET = usize{9};
    constexpr auto DIRENT_NAME_OFFSET = usize{10};
} // namespace

namespace detail {
    namespace {
        /// A node's layout, from its first byte: the leaf flag at 0, the entry
        /// count at 2, the next-leaf pointer at 8, the slots from 16.
        constexpr auto LEAF_FLAG_OFFSET = usize{0};
        constexpr auto COUNT_OFFSET = usize{2};
        constexpr auto NEXT_LEAF_OFFSET = usize{8};
        constexpr auto SLOTS_OFFSET = usize{16};

        constexpr auto LEAF_FLAG = u8{1};

        /// A superblock slot's fields, after the magic: the generation, the
        /// CRC32 over the slot's first bytes with the CRC field zeroed, then
        /// the volume's state.
        constexpr auto SUPER_GENERATION_OFFSET = usize{8};
        constexpr auto SUPER_CRC_OFFSET = usize{16};
        constexpr auto SUPER_CRC_BYTES = usize{128};
        constexpr auto SUPER_BLOCKS_OFFSET = usize{24};
        constexpr auto SUPER_ROOT_OFFSET = usize{32};
        constexpr auto SUPER_HEIGHT_OFFSET = usize{40};
        constexpr auto SUPER_BITMAP_FIRST_OFFSET = usize{48};
        constexpr auto SUPER_BITMAP_BLOCKS_OFFSET = usize{56};
        constexpr auto SUPER_NEXT_OBJECT_OFFSET = usize{64};

    } // namespace

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

    auto store_superblock(std::span<u8> block, Volume const &volume) -> void {
        store_le64(block, SUPER_GENERATION_OFFSET, volume.generation);
        store_le64(block, SUPER_BLOCKS_OFFSET, volume.blocks);
        store_le64(block, SUPER_ROOT_OFFSET, volume.root);
        block[SUPER_HEIGHT_OFFSET] = volume.height;
        store_le64(block, SUPER_BITMAP_FIRST_OFFSET, volume.bitmap_first);
        store_le32(block, SUPER_BITMAP_BLOCKS_OFFSET, volume.bitmap_blocks);
        store_le64(block, SUPER_NEXT_OBJECT_OFFSET, volume.next_object);
        // The CRC vouches for the slot's first bytes, its own field zeroed
        // while it is computed -- the GPT header's own rule.
        store_le32(block, SUPER_CRC_OFFSET, 0);
        auto const crc = ours_gpt::crc32(block.first(SUPER_CRC_BYTES));
        store_le32(block, SUPER_CRC_OFFSET, crc);
    }

    auto load_superblock(std::span<u8 const> block) -> ustl::Result<Volume, ours::Status> {
        if (std::string_view{reinterpret_cast<char const *>(block.data()), MAGIC.size()} != MAGIC) {
            return ustl::err(ours::Status::NotFound);
        }
        auto volume = Volume{};
        volume.generation = load_le64(block, SUPER_GENERATION_OFFSET);
        volume.blocks = load_le64(block, SUPER_BLOCKS_OFFSET);
        volume.root = load_le64(block, SUPER_ROOT_OFFSET);
        volume.height = block[SUPER_HEIGHT_OFFSET];
        volume.bitmap_first = load_le64(block, SUPER_BITMAP_FIRST_OFFSET);
        volume.bitmap_blocks = load_le32(block, SUPER_BITMAP_BLOCKS_OFFSET);
        volume.next_object = load_le64(block, SUPER_NEXT_OBJECT_OFFSET);
        auto const recorded = load_le32(block, SUPER_CRC_OFFSET);
        auto checking = std::array<u8, BLOCK_BYTES>{};
        std::memcpy(checking.data(), block.data(), BLOCK_BYTES);
        store_le32(checking, SUPER_CRC_OFFSET, 0);
        if (ours_gpt::crc32(std::span{checking}.first(SUPER_CRC_BYTES)) != recorded) {
            return ustl::err(ours::Status::BadState);
        }
        if (volume.root != 0 && volume.height == 0) {
            return ustl::err(ours::Status::BadState);
        }
        return ustl::ok(volume);
    }

    // --- The node. ---

    Node::Node(std::span<u8> bytes) : m_bytes(bytes) {
        m_leaf = m_bytes[LEAF_FLAG_OFFSET] == LEAF_FLAG;
        m_count = usize{load_le16(m_bytes, COUNT_OFFSET)};
    }

    auto Node::leaf(std::span<u8> bytes) -> Node {
        bytes[LEAF_FLAG_OFFSET] = LEAF_FLAG;
        bytes[COUNT_OFFSET] = 0;
        return Node{bytes};
    }

    auto Node::internal(std::span<u8> bytes) -> Node {
        bytes[LEAF_FLAG_OFFSET] = 0;
        bytes[COUNT_OFFSET] = 0;
        return Node{bytes};
    }

    auto Node::bytes() const -> std::span<u8> {
        return m_bytes;
    }

    auto Node::is_leaf() const -> bool {
        return m_leaf;
    }

    auto Node::count() const -> usize {
        return m_count;
    }

    auto Node::store_count() -> void {
        store_le16(m_bytes, COUNT_OFFSET, static_cast<u16>(m_count));
    }

    auto Node::next_leaf() const -> u64 {
        return load_le64(m_bytes, NEXT_LEAF_OFFSET);
    }

    auto Node::set_next_leaf(u64 block) -> void {
        store_le64(m_bytes, NEXT_LEAF_OFFSET, block);
    }

    auto Node::m_leafslot_offset(usize slot) -> usize {
        return SLOTS_OFFSET + slot * (KEY_BYTES + VALUE_BYTES);
    }

    auto Node::internal_slot_offset(usize slot) -> usize {
        return SLOTS_OFFSET + slot * (KEY_BYTES + 8);
    }

    auto Node::key(usize slot) const -> Key {
        auto const at = m_leaf ? m_leafslot_offset(slot) : internal_slot_offset(slot) + 8;
        auto const word = load_le64(m_bytes, at + 8);
        return Key{.object = load_le64(m_bytes, at),
                   .type = unpack_type(word),
                   .offset = unpack_offset(word)};
    }

    auto Node::set_key(usize slot, Key const &key) -> void {
        auto const at = m_leaf ? m_leafslot_offset(slot) : internal_slot_offset(slot) + 8;
        store_le64(m_bytes, at, key.object);
        store_le64(m_bytes, at + 8, pack_type_offset(key.type, key.offset));
    }

    auto Node::child(usize slot) const -> u64 {
        return load_le64(m_bytes, internal_slot_offset(slot));
    }

    auto Node::set_child(usize slot, u64 child) -> void {
        store_le64(m_bytes, internal_slot_offset(slot), child);
    }

    auto Node::value(usize slot) const -> Value {
        auto value = Value{};
        std::memcpy(value.data(), m_bytes.data() + m_leafslot_offset(slot) + KEY_BYTES,
                    VALUE_BYTES);
        return value;
    }

    auto Node::set_value(usize slot, Value const &value) -> void {
        std::memcpy(m_bytes.data() + m_leafslot_offset(slot) + KEY_BYTES, value.data(),
                    VALUE_BYTES);
    }

    auto Node::upper_bound(Key const &target) const -> usize {
        auto low = usize{0};
        auto high = m_count;
        while (low < high) {
            auto const mid = low + (high - low) / 2;
            if (key(mid) <= target) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        return low;
    }

    auto Node::child_for(Key const &key) const -> u64 {
        return child(upper_bound(key));
    }

    auto Node::insert_leaf(usize at, Key const &key, Value const &value) -> void {
        auto const slot = m_leafslot_offset(at);
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;
        std::memmove(m_bytes.data() + slot + slot_bytes, m_bytes.data() + slot,
                     (m_count - at) * slot_bytes);
        set_key(at, key);
        set_value(at, value);
        m_count += 1;
        store_count();
    }

    auto Node::insert_child(usize at, Key const &key, u64 child) -> void {
        // The keys from |at| move one slot right, the children from |at| + 1
        // with them: the new child lands after the child that covered the
        // range the new key now splits.
        auto const slot_bytes = KEY_BYTES + 8;
        auto const from = internal_slot_offset(at) + 8;
        std::memmove(m_bytes.data() + from + slot_bytes, m_bytes.data() + from,
                     (m_count - at) * slot_bytes);
        set_key(at, key);
        set_child(at + 1, child);
        m_count += 1;
        store_count();
    }

    auto Node::erase_leaf(usize at) -> void {
        auto const slot = m_leafslot_offset(at);
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;
        std::memmove(m_bytes.data() + slot, m_bytes.data() + slot + slot_bytes,
                     (m_count - at - 1) * slot_bytes);
        m_count -= 1;
        store_count();
    }

    auto Node::erase_child(usize at) -> void {
        // The key at |at| and the child at |at| + 1 go: everything from the
        // key's position shifts one slot left, the node's last child with it.
        auto const slot_bytes = KEY_BYTES + 8;
        auto const from = internal_slot_offset(at) + 8;
        std::memmove(m_bytes.data() + from, m_bytes.data() + from + slot_bytes,
                     (m_count - at) * slot_bytes);
        m_count -= 1;
        store_count();
    }

    auto Node::capacity() const -> usize {
        return m_leaf ? LEAF_CAPACITY : INTERNAL_CAPACITY;
    }

    auto Node::minimum() const -> usize {
        return m_leaf ? LEAF_MIN : INTERNAL_MIN;
    }

    auto Node::take_upper_half(Node &left) -> Key {
        auto const keep = left.m_count / 2;
        auto const moved = left.m_count - keep;

        // The upper half of the items moves whole, and the chain link follows:
        // the caller re-links |left| to this node's block once it has one.
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;
        std::memcpy(m_bytes.data() + m_leafslot_offset(0),
                    left.m_bytes.data() + m_leafslot_offset(keep), moved * slot_bytes);
        set_next_leaf(left.next_leaf());
        m_count = moved;
        store_count();
        left.m_count = keep;
        left.store_count();
        return key(0);
    }

    auto Node::take_upper_half_internal(Node &left) -> Key {
        auto const mid = left.m_count / 2;
        auto const moved = left.m_count - mid - 1;

        // This node's slot j takes the old child mid+1+j and the old key
        // mid+1+j, with the old child count as its one child more than its
        // keys; the key at |mid| is answered and stays in neither node.
        auto const slot_bytes = KEY_BYTES + 8;
        std::memcpy(m_bytes.data() + internal_slot_offset(0),
                    left.m_bytes.data() + internal_slot_offset(mid + 1), moved * slot_bytes);
        set_child(moved, left.child(left.m_count));
        m_count = moved;
        store_count();
        left.m_count = mid;
        left.store_count();
        return left.key(mid);
    }

    auto Node::borrow_last_from(Node &left) -> Key {
        // The left sibling's last item moves to the front: this node's slots
        // shift right one to open the slot, and the moved key is what the
        // parent now routes the pair by.
        m_count += 1;
        store_count();
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;
        std::memmove(m_bytes.data() + m_leafslot_offset(1), m_bytes.data() + m_leafslot_offset(0),
                     (m_count - 1) * slot_bytes);
        std::memcpy(m_bytes.data() + m_leafslot_offset(0),
                    left.m_bytes.data() + m_leafslot_offset(left.m_count - 1), slot_bytes);
        auto const moved = left.key(left.m_count - 1);
        left.m_count -= 1;
        left.store_count();
        return moved;
    }

    auto Node::borrow_last_from_internal(Node &left, Key const &separator) -> Key {
        // The left sibling's last child and last key move over: the child in
        // front, the parent's separator as this node's first key, and the
        // left's old last key up to the parent.
        m_count += 1;
        store_count();
        auto const slot_bytes = KEY_BYTES + 8;
        std::memmove(m_bytes.data() + internal_slot_offset(1),
                     m_bytes.data() + internal_slot_offset(0), (m_count - 1) * slot_bytes + 8);
        set_child(0, left.child(left.m_count));
        set_key(0, separator);
        auto const moved = left.key(left.m_count - 1);
        left.m_count -= 1;
        left.store_count();
        return moved;
    }

    auto Node::borrow_first_from(Node &right) -> Key {
        // The right sibling's first item moves to the back, and the right's new
        // first key is what the parent routes the pair by.
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;
        std::memcpy(m_bytes.data() + m_leafslot_offset(m_count),
                    right.m_bytes.data() + m_leafslot_offset(0), slot_bytes);
        m_count += 1;
        store_count();
        std::memmove(right.m_bytes.data() + m_leafslot_offset(0),
                     right.m_bytes.data() + m_leafslot_offset(1), (right.m_count - 1) * slot_bytes);
        right.m_count -= 1;
        right.store_count();
        return right.key(0);
    }

    auto Node::borrow_first_from_internal(Node &right, Key const &separator) -> Key {
        // The right sibling's first child and first key move over: the parent's
        // separator as this node's new last key, the right's old first key up
        // to the parent, and the right's second child becomes its first.
        set_child(m_count + 1, right.child(0));
        set_key(m_count, separator);
        m_count += 1;
        store_count();
        auto const moved = right.key(0);
        auto const slot_bytes = KEY_BYTES + 8;
        std::memmove(right.m_bytes.data() + internal_slot_offset(0),
                     right.m_bytes.data() + internal_slot_offset(1),
                     (right.m_count - 1) * slot_bytes + 8);
        right.m_count -= 1;
        right.store_count();
        return moved;
    }

    auto Node::absorb_leaf(Node &right) -> void {
        auto const slot_bytes = KEY_BYTES + VALUE_BYTES;
        std::memcpy(m_bytes.data() + m_leafslot_offset(m_count),
                    right.m_bytes.data() + m_leafslot_offset(0), right.m_count * slot_bytes);
        m_count += right.m_count;
        store_count();
        // The chain: what followed |right| follows this node now.
        set_next_leaf(right.next_leaf());
        right.m_count = 0;
        right.store_count();
    }

    auto Node::absorb_internal(Node &right, Key const &separator) -> void {
        // The parent's separator descends as the pair's key, and the right's
        // slots follow whole -- slot for slot, the right's first child among
        // them, its last child as this node's new last.
        set_key(m_count, separator);
        auto const slot_bytes = KEY_BYTES + 8;
        std::memcpy(m_bytes.data() + internal_slot_offset(m_count + 1),
                    right.m_bytes.data() + internal_slot_offset(0), right.m_count * slot_bytes);
        set_child(m_count + 1 + right.m_count, right.child(right.m_count));
        m_count += right.m_count + 1;
        store_count();
        right.m_count = 0;
        right.store_count();
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
    detail::store_le64(out, INODE_SIZE_OFFSET, size);
    out[INODE_KIND_OFFSET] = std::to_underlying(kind);
}

auto decode_inode_value(Value const &value) -> ustl::Result<InodeValue, ours::Status> {
    auto const kind = static_cast<ObjectKind>(value[INODE_KIND_OFFSET]);
    if (kind != ObjectKind::File && kind != ObjectKind::Directory) {
        return ustl::err(ours::Status::InvArgs);
    }
    return ustl::ok(InodeValue{.size = detail::load_le64(value, INODE_SIZE_OFFSET), .kind = kind});
}

auto encode_dirent_value(Value &out, u64 child, ObjectKind kind, std::string_view name)
    -> ours::Status {
    if (name.size() > MAX_NAME_BYTES) {
        return ours::Status::InvArgs;
    }
    detail::store_le64(out, DIRENT_CHILD_OFFSET, child);
    out[DIRENT_KIND_OFFSET] = std::to_underlying(kind);
    out[DIRENT_NAME_LENGTH_OFFSET] = static_cast<u8>(name.size());
    std::memcpy(out.data() + DIRENT_NAME_OFFSET, name.data(), name.size());
    return ours::Status::Ok;
}

auto decode_dirent_value(Value const &value) -> ustl::Result<DirentValue, ours::Status> {
    auto const kind = static_cast<ObjectKind>(value[DIRENT_KIND_OFFSET]);
    auto const length = usize{value[DIRENT_NAME_LENGTH_OFFSET]};
    if ((kind != ObjectKind::File && kind != ObjectKind::Directory) || length > MAX_NAME_BYTES) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto const name =
        std::string_view{reinterpret_cast<char const *>(value.data()) + DIRENT_NAME_OFFSET, length};
    return ustl::ok(DirentValue{
        .child = detail::load_le64(value, DIRENT_CHILD_OFFSET), .kind = kind, .name = name});
}

auto encode_data_value(Value &out, u64 block) -> void {
    detail::store_le64(out, DIRENT_CHILD_OFFSET, block);
}

auto decode_data_value(Value const &value) -> u64 {
    return detail::load_le64(value, DIRENT_CHILD_OFFSET);
}
} // namespace ours_btreefs
