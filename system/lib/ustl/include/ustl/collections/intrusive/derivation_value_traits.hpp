/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DERIVATION_VALUE_TRAITS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DERIVATION_VALUE_TRAITS_HPP 1

#include <utility>

#include <ustl/collections/intrusive/intrusive_fwd.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

namespace ustl::collections::intrusive {
    template <typename T, typename TNodeTraits, LinkModeType Mode>
    struct DerivationValueTraits final {
        using NodeTraits = TNodeTraits;
        using Element = T;
        using Node = typename NodeTraits::Node;
        using NodePtr = typename NodeTraits::NodePtr;
        using ConstNodePtr = typename NodeTraits::ConstNodePtr;
        using PtrMut = typename PointerTraits<NodePtr>::template Rebind<Element>;
        using Ptr = typename PointerTraits<NodePtr>::template Rebind<Element const>;
        using RefMut = decltype(*std::declval<PtrMut>());
        using Ref = decltype(*std::declval<Ptr>());
        static constexpr LinkModeType LINK_MODE = Mode;

        [[nodiscard]] static constexpr auto to_node_ptr(RefMut value) noexcept -> NodePtr {
            return PointerTraits<NodePtr>::pointer_to(static_cast<Node &>(value));
        }

        [[nodiscard]] static constexpr auto to_node_ptr(Ref value) noexcept -> ConstNodePtr {
            return PointerTraits<ConstNodePtr>::pointer_to(static_cast<Node const &>(value));
        }

        [[nodiscard]] static constexpr auto to_value_ptr(NodePtr const node) noexcept -> PtrMut {
            return PointerTraits<PtrMut>::pointer_to(static_cast<Element &>(*node));
        }

        [[nodiscard]] static constexpr auto to_value_ptr(ConstNodePtr const node) noexcept -> Ptr {
            return PointerTraits<Ptr>::pointer_to(static_cast<Element const &>(*node));
        }
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_DERIVATION_VALUE_TRAITS_HPP
