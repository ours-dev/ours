/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_VALUE_TRAITS_ACCESS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_VALUE_TRAITS_ACCESS_HPP 1

#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

#include <utility>

namespace ustl::collections::intrusive::detail {
template <typename T, typename Traits>
struct ValueTraitsAccess final {
    using NodeTraits = typename Traits::NodeTraits;
    using Node = typename NodeTraits::Node;
    using NodePtr = typename NodeTraits::NodePtr;
    using ConstNodePtr = typename NodeTraits::ConstNodePtr;

    [[nodiscard]]
    static constexpr auto to_node(T &value) noexcept -> NodePtr {
        if constexpr (requires(T &candidate) { Traits::to_node_ptr(candidate); }) {
            return Traits::to_node_ptr(value);
        } else {
            return static_cast<Traits &>(value).this_ptr();
        }
    }

    [[nodiscard]]
    static constexpr auto to_node(T const &value) noexcept -> ConstNodePtr {
        if constexpr (requires(T const &candidate) { Traits::to_node_ptr(candidate); }) {
            return Traits::to_node_ptr(value);
        } else {
            return static_cast<Traits const &>(value).this_ptr();
        }
    }

    [[nodiscard]]
    static constexpr auto to_value(NodePtr const node) noexcept -> T * {
        if constexpr (requires(NodePtr candidate) { Traits::to_value_ptr(candidate); }) {
            return PointerTraits<decltype(Traits::to_value_ptr(node))>::to_raw_pointer(
                Traits::to_value_ptr(node));
        } else {
            auto *const raw_node = PointerTraits<NodePtr>::to_raw_pointer(node);
            auto *const hook = static_cast<Traits *>(raw_node);
            return static_cast<T *>(hook);
        }
    }

    [[nodiscard]]
    static constexpr auto to_value(ConstNodePtr const node) noexcept -> T const * {
        if constexpr (requires(ConstNodePtr candidate) { Traits::to_value_ptr(candidate); }) {
            return PointerTraits<decltype(Traits::to_value_ptr(node))>::to_raw_pointer(
                Traits::to_value_ptr(node));
        } else {
            auto const *const raw_node = PointerTraits<ConstNodePtr>::to_raw_pointer(node);
            auto const *const hook = static_cast<Traits const *>(raw_node);
            return static_cast<T const *>(hook);
        }
    }
};

template <typename T, typename Traits>
struct ContainerValueTraits final {
    using Access = ValueTraitsAccess<T, Traits>;
    using NodeTraits = typename Access::NodeTraits;
    using Node = typename Access::Node;
    using NodePtr = typename Access::NodePtr;
    using ConstNodePtr = typename Access::ConstNodePtr;
    using Element = T;
    using PtrMut = typename PointerTraits<NodePtr>::template Rebind<T>;
    using Ptr = typename PointerTraits<NodePtr>::template Rebind<T const>;
    using RefMut = decltype(*std::declval<PtrMut>());
    using Ref = decltype(*std::declval<Ptr>());
    static constexpr bool IS_STATEFUL_VALUE_TRAITS = false;
    static constexpr LinkModeType LINK_MODE = []() constexpr {
        if constexpr (requires { Traits::LINK_MODE; }) {
            return Traits::LINK_MODE;
        } else {
            return Traits::HookTags::LINK_MODE;
        }
    }();

    [[nodiscard]]
    static constexpr auto to_node_ptr(RefMut value) noexcept -> NodePtr {
        return Access::to_node(value);
    }

    [[nodiscard]]
    static constexpr auto to_node_ptr(Ref value) noexcept -> ConstNodePtr {
        return Access::to_node(value);
    }

    [[nodiscard]]
    static constexpr auto to_value_ptr(NodePtr const node) noexcept -> PtrMut {
        return PointerTraits<PtrMut>::pointer_to(*Access::to_value(node));
    }

    [[nodiscard]]
    static constexpr auto to_value_ptr(ConstNodePtr const node) noexcept -> Ptr {
        return PointerTraits<Ptr>::pointer_to(*Access::to_value(node));
    }
};
} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_VALUE_TRAITS_ACCESS_HPP
