/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_HASHTABLE_NODE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_HASHTABLE_NODE_HPP 1

#include <ustl/collections/intrusive/pointer_rebind.hpp>

#include <cstddef>

namespace ustl::collections::intrusive::detail {
template <typename VoidPointer, bool StoreHash, bool OptimizeMultikey>
struct HashNode {
    using NodePtr = typename PointerRebind<VoidPointer, HashNode>::Type;

    NodePtr m_next{};
    std::size_t m_hash{};
    /// Whether the node is in a bucket. m_next cannot say it, being null
    /// both for an unlinked node and for the last node of a chain; and it is
    /// the node's only yes/no fact, so it has no word to share.
    bool m_linked{};
};

template <typename VoidPointer, bool StoreHash, bool OptimizeMultikey>
struct HashNodeTraits final {
    using Node = HashNode<VoidPointer, StoreHash, OptimizeMultikey>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename PointerRebind<VoidPointer, Node const>::Type;

    static constexpr bool STORE_HASH = StoreHash;
    static constexpr bool OPTIMIZE_MULTIKEY = OptimizeMultikey;

    [[nodiscard]]
    static constexpr auto get_next(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_next;
    }
    static constexpr auto set_next(NodePtr const node, NodePtr const next) noexcept -> void {
        node->m_next = next;
    }
    [[nodiscard]]
    static constexpr auto get_hash(ConstNodePtr const node) noexcept -> std::size_t {
        return node->m_hash;
    }
    static constexpr auto set_hash(NodePtr const node, std::size_t const hash) noexcept -> void {
        node->m_hash = hash;
    }
    [[nodiscard]]
    static constexpr auto is_linked(ConstNodePtr const node) noexcept -> bool {
        return node->m_linked;
    }
    static constexpr auto set_linked(NodePtr const node, bool const linked) noexcept -> void {
        node->m_linked = linked;
    }

    static constexpr auto init(NodePtr const node) noexcept -> void {
        node->m_next = NodePtr{};
        node->m_hash = 0U;
        node->m_linked = false;
    }
};

template <typename NodeTraits>
struct HashBucket final {
    using NodePtr = typename NodeTraits::NodePtr;

    NodePtr m_head{};
};
} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_HASHTABLE_NODE_HPP
