/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_SLIST_NODE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_SLIST_NODE_HPP 1

#include <ustl/collections/intrusive/pointer_rebind.hpp>

namespace ustl::collections::intrusive {
template <typename VoidPointer>
struct SlistNode {
    using NodePtr = typename PointerRebind<VoidPointer, SlistNode>::Type;

    NodePtr m_next;
};

template <typename VoidPointer>
struct SlistNodeTraits final {
    using Node = SlistNode<VoidPointer>;
    using NodePtr = typename Node::NodePtr;
    using ConstNodePtr = typename PointerRebind<VoidPointer, const Node>::Type;

    [[nodiscard]]
    static constexpr auto get_next(ConstNodePtr const node) noexcept -> NodePtr {
        return node->m_next;
    }

    [[nodiscard]]
    static constexpr auto get_next(NodePtr const node) noexcept -> NodePtr {
        return node->m_next;
    }

    static constexpr auto set_next(NodePtr const node, NodePtr const next) noexcept -> void {
        node->m_next = next;
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_SLIST_NODE_HPP
