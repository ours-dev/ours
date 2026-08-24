/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_MEMBER_VALUE_TRAITS_HPP
#define USTL_COLLECTIONS_INTRUSIVE_MEMBER_VALUE_TRAITS_HPP 1

#include <ustl/collections/intrusive/detail/parent_from_member.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

namespace ustl::collections::intrusive {
    template <typename T, typename TNodeTraits, typename TNodeTraits::Node T::*MemberPtr,
              LinkModeType Mode>
    struct MemberValueTraits final {
        using NodeTraits = TNodeTraits;
        using Element = T;
        using Node = typename NodeTraits::Node;
        using NodePtr = typename NodeTraits::NodePtr;
        using ConstNodePtr = typename NodeTraits::ConstNodePtr;
        using PtrMut = typename PointerTraits<NodePtr>::template Rebind<Element>;
        using Ptr = typename PointerTraits<NodePtr>::template Rebind<Element const>;
        using RefMut = Element &;
        using Ref = Element const &;
        static constexpr LinkModeType LINK_MODE = Mode;

        [[nodiscard]] static constexpr auto to_node_ptr(RefMut value) noexcept -> NodePtr {
            return PointerTraits<NodePtr>::pointer_to(value.*MemberPtr);
        }

        [[nodiscard]] static constexpr auto to_node_ptr(Ref value) noexcept -> ConstNodePtr {
            return PointerTraits<ConstNodePtr>::pointer_to(value.*MemberPtr);
        }

        [[nodiscard]] static auto to_value_ptr(NodePtr const node) noexcept -> PtrMut {
            auto *const raw_node = PointerTraits<NodePtr>::to_raw_pointer(node);
            return PointerTraits<PtrMut>::pointer_to(*detail::parent_from_member<Element, Node>(raw_node, MemberPtr));
        }

        [[nodiscard]] static auto to_value_ptr(ConstNodePtr const node) noexcept -> Ptr {
            auto const *const raw_node = PointerTraits<ConstNodePtr>::to_raw_pointer(node);
            return PointerTraits<Ptr>::pointer_to(
                *detail::parent_from_member<Element, Node>(raw_node, MemberPtr));
        }
    };

    template <typename T, typename Hook, Hook T::*MemberPtr>
    struct Mhtraits final {
        using HookType = Hook;
        using NodeTraits = typename HookType::HookTags::NodeTraits;
        using Node = typename NodeTraits::Node;
        using Element = T;
        using NodePtr = typename NodeTraits::NodePtr;
        using ConstNodePtr = typename NodeTraits::ConstNodePtr;
        using PtrMut = typename PointerTraits<NodePtr>::template Rebind<Element>;
        using Ptr = typename PointerTraits<NodePtr>::template Rebind<Element const>;
        using RefMut = Element &;
        using Ref = Element const &;
        static constexpr LinkModeType LINK_MODE = HookType::HookTags::LINK_MODE;

        [[nodiscard]] static constexpr auto to_node_ptr(RefMut value) noexcept -> NodePtr {
            auto &hook = value.*MemberPtr;
            return PointerTraits<NodePtr>::pointer_to(static_cast<Node &>(hook));
        }

        [[nodiscard]] static constexpr auto to_node_ptr(Ref value) noexcept -> ConstNodePtr {
            auto const &hook = value.*MemberPtr;
            return PointerTraits<ConstNodePtr>::pointer_to(static_cast<Node const &>(hook));
        }

        [[nodiscard]] static auto to_value_ptr(NodePtr const node) noexcept -> PtrMut {
            auto *const raw_node = PointerTraits<NodePtr>::to_raw_pointer(node);
            auto *const hook = static_cast<HookType *>(raw_node);
            return PointerTraits<PtrMut>::pointer_to(*detail::parent_from_member<Element, HookType>(hook, MemberPtr));
        }

        [[nodiscard]] static auto to_value_ptr(ConstNodePtr const node) noexcept -> Ptr {
            auto const *const raw_node = PointerTraits<ConstNodePtr>::to_raw_pointer(node);
            auto const *const hook = static_cast<HookType const *>(raw_node);
            return PointerTraits<Ptr>::pointer_to(
                *detail::parent_from_member<Element, HookType>(hook, MemberPtr));
        }
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_MEMBER_VALUE_TRAITS_HPP
