/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_RADIX_TREE_HPP
#define GKTL_RADIX_TREE_HPP 1

#include <memory>

#include <gktl/types.hpp>

namespace gktl {

template <class T, class A, class KofT, class KeyEncoder>
class RadixTree final {
  public:
    using Value = T;
    using Allocator = A;
    using KeyOfValue = KofT;
    using Encoder = KeyEncoder;

    static constexpr auto NUM_CHILDREN = Encoder::NUM_CHILDREN;
    static constexpr auto BITS_PER_CODE = Encoder::BITS_PER_CODE;
    static constexpr auto CODES_PER_KEY = Encoder::CODES_PER_KEY;
    static constexpr auto KEY_BITS = CODES_PER_KEY * BITS_PER_CODE;
    static constexpr auto KEY_MASK = [] {
        if constexpr (KEY_BITS >= 64) {
            return ~0_u64;
        } else {
            return (1_u64 << KEY_BITS) - 1;
        }
    }();

    RadixTree() = default;

    explicit RadixTree(Allocator const &allocator, KeyOfValue key_of = {}, Encoder encoder = {})
        : m_node_allocator(allocator), m_key_of(key_of), m_encoder(encoder) {}

    RadixTree(RadixTree const &) = delete;
    auto operator=(RadixTree const &) -> RadixTree & = delete;

    ~RadixTree() {
        clear();
    }

    auto reserve(usize capacity) -> bool {
        if (m_nodes != nullptr) {
            return capacity <= m_capacity;
        }

        if (capacity == 0) {
            capacity = 1;
        }

        m_nodes = NodeAllocatorTraits::allocate(m_node_allocator, capacity);
        if (m_nodes == nullptr) {
            return false;
        }

        m_capacity = capacity;
        return alloc_node(0, 0) != nullptr;
    }

    auto clear() -> void {
        if (m_nodes == nullptr) {
            return;
        }

        for (usize i = 0; i < m_size; ++i) {
            NodeAllocatorTraits::destroy(m_node_allocator, &m_nodes[i]);
        }
        NodeAllocatorTraits::deallocate(m_node_allocator, m_nodes, m_capacity);

        m_nodes = nullptr;
        m_capacity = 0;
        m_size = 0;
    }

    auto insert(Value const &value) -> bool {
        auto const key = m_key_of(value);
        auto const max_nodes = max_required_nodes_for_key(key);
        if (m_nodes == nullptr || max_nodes == 0 || m_size + max_nodes > m_capacity) {
            return false;
        }

        auto offset = 0_usize;
        auto *node = root();
        while (!key_is_end(key, offset)) {
            auto const code = encode_at(key, offset);
            auto const slot = child_slot(code);
            if (code == 0 || slot >= NUM_CHILDREN) {
                return false;
            }

            auto *child = node->children[slot];
            if (child == nullptr) {
                return append_suffix(node, key, offset, value);
            }

            auto const matched = match_edge(child, key, offset);
            if (matched == child->length) {
                node = child;
                offset += matched;
                continue;
            }

            return split_child(node, child, matched, key, offset + matched, value);
        }

        mark_value(node, value);
        return true;
    }

    template <class Key>
    [[nodiscard]]
    auto find(Key const key) const -> Value const * {
        if (key_is_null(key) || m_nodes == nullptr) {
            return nullptr;
        }

        auto offset = 0_usize;
        auto const *node = root();
        while (!key_is_end(key, offset)) {
            auto const code = encode_at(key, offset);
            auto const slot = child_slot(code);
            if (code == 0 || slot >= NUM_CHILDREN || node->children[slot] == nullptr) {
                return nullptr;
            }

            auto const *child = node->children[slot];
            auto const matched = match_edge(child, key, offset);
            if (matched != child->length) {
                return nullptr;
            }

            node = child;
            offset += matched;
        }

        return node->value;
    }

    [[nodiscard]]
    auto max_required_nodes(Value const &value) const -> usize {
        return max_required_nodes_for_key(m_key_of(value));
    }

    template <class Key>
    [[nodiscard]]
    auto max_required_nodes_for_key(Key const key) const -> usize {
        if (key_is_null(key)) {
            return 0;
        }

        auto count = 0_usize;
        while (!key_is_end(key, count)) {
            if (encode_at(key, count) == 0) {
                return 0;
            }
            ++count;
        }
        return count;
    }

  private:
    struct Node {
        u64 key{};
        usize length{};
        Value const *value{};
        Node *children[NUM_CHILDREN]{};

        Node() = default;

        Node(u64 key, usize length) : key(key & KEY_MASK), length(length) {}
    };

    using NodeAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
    using NodeAllocatorTraits = std::allocator_traits<NodeAllocator>;

    template <class Key>
    [[nodiscard]]
    static auto key_is_null(Key const key) -> bool {
        if constexpr (requires { key == nullptr; }) {
            return key == nullptr;
        } else {
            return false;
        }
    }

    template <class Key>
    [[nodiscard]]
    static auto key_byte(Key const key, usize offset) -> u64 {
        return static_cast<u64>(key[offset]);
    }

    template <class Key>
    [[nodiscard]]
    static auto key_is_end(Key const key, usize offset) -> bool {
        return key_byte(key, offset) == 0;
    }

    [[nodiscard]]
    static auto get_code(u64 key, usize index) -> u64 {
        return (key >> (index * BITS_PER_CODE)) & ((1_u64 << BITS_PER_CODE) - 1);
    }

    [[nodiscard]]
    static auto mask_for_length(usize length) -> u64 {
        auto const bits = length * BITS_PER_CODE;
        if (bits >= 64) {
            return ~0_u64;
        }
        return (1_u64 << bits) - 1;
    }

    [[nodiscard]]
    static auto slice_key(u64 key, usize offset, usize length) -> u64 {
        auto result = 0_u64;
        for (usize i = 0; i < length; ++i) {
            result |= get_code(key, offset + i) << (i * BITS_PER_CODE);
        }
        return result;
    }

    [[nodiscard]]
    static auto child_slot(u64 code) -> usize {
        return static_cast<usize>(code);
    }

    [[nodiscard]]
    static auto child_slot_from_key(u64 key) -> usize {
        return child_slot(get_code(key, 0));
    }

    template <class Key>
    [[nodiscard]]
    auto encode_at(Key const key, usize offset) const -> u64 {
        return m_encoder(key_byte(key, offset));
    }

    [[nodiscard]]
    auto root() -> Node * {
        return m_nodes;
    }

    [[nodiscard]]
    auto root() const -> Node const * {
        return m_nodes;
    }

    auto alloc_node(u64 key, usize length) -> Node * {
        if (m_size >= m_capacity) {
            return nullptr;
        }

        auto *node = &m_nodes[m_size++];
        NodeAllocatorTraits::construct(m_node_allocator, node, key, length);
        return node;
    }

    auto mark_value(Node *node, Value const &value) -> void {
        if (node->value == nullptr) {
            node->value = &value;
        }
    }

    template <class Key>
    [[nodiscard]]
    auto make_edge(Key const key, usize offset, usize *length) const -> u64 {
        auto result = 0_u64;
        *length = 0;
        while (*length < CODES_PER_KEY && !key_is_end(key, offset + *length)) {
            auto const code = encode_at(key, offset + *length);
            if (code == 0) {
                *length = 0;
                return 0;
            }

            result |= code << (*length * BITS_PER_CODE);
            ++(*length);
        }
        return result;
    }

    template <class Key>
    [[nodiscard]]
    auto match_edge(Node const *node, Key const key, usize offset) const -> usize {
        for (usize i = 0; i < node->length; ++i) {
            if (key_is_end(key, offset + i)) {
                return i;
            }

            auto const code = encode_at(key, offset + i);
            if (code == 0 || code != get_code(node->key, i)) {
                return i;
            }
        }
        return node->length;
    }

    template <class Key>
    auto append_suffix(Node *parent, Key const key, usize offset, Value const &value) -> bool {
        auto *node = parent;
        while (!key_is_end(key, offset)) {
            usize length = 0;
            auto const edge = make_edge(key, offset, &length);
            if (length == 0) {
                return false;
            }

            auto const slot = child_slot_from_key(edge);
            if (slot >= NUM_CHILDREN || node->children[slot] != nullptr) {
                return false;
            }

            auto *child = alloc_node(edge, length);
            if (child == nullptr) {
                return false;
            }

            node->children[slot] = child;
            node = child;
            offset += length;
        }

        mark_value(node, value);
        return true;
    }

    template <class Key>
    auto split_child(Node *parent, Node *child, usize prefix_length, Key const key,
                     usize suffix_offset, Value const &value) -> bool {
        if (prefix_length == 0) {
            return false;
        }

        auto const old_key = child->key;
        auto const old_length = child->length;
        auto const prefix_key = old_key & mask_for_length(prefix_length);

        auto *split = alloc_node(prefix_key, prefix_length);
        if (split == nullptr) {
            return false;
        }

        child->key = slice_key(old_key, prefix_length, old_length - prefix_length);
        child->length = old_length - prefix_length;

        parent->children[child_slot_from_key(prefix_key)] = split;
        split->children[child_slot_from_key(child->key)] = child;

        if (key_is_end(key, suffix_offset)) {
            mark_value(split, value);
            return true;
        }
        return append_suffix(split, key, suffix_offset, value);
    }

    [[no_unique_address]]
    NodeAllocator m_node_allocator{};
    [[no_unique_address]]
    KeyOfValue m_key_of{};
    [[no_unique_address]]
    Encoder m_encoder{};
    Node *m_nodes{};
    usize m_capacity{};
    usize m_size{};
};

} // namespace gktl

#endif // GKTL_RADIX_TREE_HPP
