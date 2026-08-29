/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_TRIVIAL_VALUE_TRAITS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_TRIVIAL_VALUE_TRAITS_HPP 1

#include <ustl/collections/intrusive/intrusive_fwd.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

namespace ustl::collections::intrusive {
template <typename TNodeTraits, LinkModeType Mode>
struct TrivialValueTraits final {
    using NodeTraits = TNodeTraits;
    using Element = typename NodeTraits::Node;
    using NodePtr = typename NodeTraits::NodePtr;
    using ConstNodePtr = typename NodeTraits::ConstNodePtr;
    using PtrMut = NodePtr;
    using Ptr = ConstNodePtr;
    static constexpr LinkModeType LINK_MODE = Mode;

    [[nodiscard]]
    static constexpr auto to_node_ptr(Element &value) noexcept -> NodePtr {
        return PointerTraits<NodePtr>::pointer_to(value);
    }

    [[nodiscard]]
    static constexpr auto to_node_ptr(Element const &value) noexcept -> ConstNodePtr {
        return PointerTraits<ConstNodePtr>::pointer_to(value);
    }

    [[nodiscard]]
    static constexpr auto to_value_ptr(NodePtr const node) noexcept -> PtrMut {
        return node;
    }

    [[nodiscard]]
    static constexpr auto to_value_ptr(ConstNodePtr const node) noexcept -> Ptr {
        return node;
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_TRIVIAL_VALUE_TRAITS_HPP
