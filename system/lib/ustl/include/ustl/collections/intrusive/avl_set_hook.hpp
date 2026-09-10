/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_AVL_SET_HOOK_HPP
#define USTL_COLLECTIONS_INTRUSIVE_AVL_SET_HOOK_HPP 1

#include <ustl/collections/intrusive/avltree_algorithms.hpp>
#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/avltree_node.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pack_options.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

#include <type_traits>

namespace ustl::collections::intrusive {
namespace detail {
    template <bool IsBaseHook, typename... Options>
    class AvlSetHookImpl: public AvltreeNodeTraits<
                              typename PackOptions<HookDefaults, Options...>::Type::VoidPointer,
                              PackOptions<HookDefaults, Options...>::Type::OPTIMIZE_SIZE>::Node {
        using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;

      public:
        using NodeTraits =
            AvltreeNodeTraits<typename PackedOptions::VoidPointer, PackedOptions::OPTIMIZE_SIZE>;

      private:
        using Node = typename NodeTraits::Node;
        using Algorithms = AvltreeAlgorithms<NodeTraits>;

      public:
        using NodePtr = typename NodeTraits::NodePtr;
        using ConstNodePtr = typename NodeTraits::ConstNodePtr;

        struct HookTags final {
            using NodeTraits = AvltreeNodeTraits<typename PackedOptions::VoidPointer,
                                                 PackedOptions::OPTIMIZE_SIZE>;
            using Tag = std::conditional_t<IsBaseHook, typename PackedOptions::Tag, MemberTag>;
            static constexpr LinkModeType LINK_MODE = PackedOptions::LINK_MODE;
            static constexpr bool IS_BASE_HOOK = IsBaseHook;
        };

        constexpr AvlSetHookImpl() noexcept {
            if constexpr (IsSafeAutounlink<PackedOptions::LINK_MODE>::VALUE) {
                Algorithms::init(this_ptr());
            }
        }

        constexpr AvlSetHookImpl(AvlSetHookImpl const &) noexcept : AvlSetHookImpl() {}

        constexpr auto operator=(AvlSetHookImpl const &) noexcept -> AvlSetHookImpl & {
            return *this;
        }

        constexpr ~AvlSetHookImpl() noexcept {
            if constexpr (PackedOptions::LINK_MODE == LinkModeType::SafeLink) {
                detail::require(!is_linked());
            } else if constexpr (PackedOptions::LINK_MODE == LinkModeType::AutoUnlink) {
                unlink();
            }
        }

        [[nodiscard]]
        constexpr auto this_ptr() noexcept -> NodePtr {
            return PointerTraits<NodePtr>::pointer_to(static_cast<Node &>(*this));
        }

        [[nodiscard]]
        constexpr auto this_ptr() const noexcept -> ConstNodePtr {
            return PointerTraits<ConstNodePtr>::pointer_to(static_cast<Node const &>(*this));
        }

        constexpr auto swap_nodes(AvlSetHookImpl &other) noexcept -> void {
            Algorithms::swap_nodes(this_ptr(), other.this_ptr());
        }

        [[nodiscard]]
        constexpr auto is_linked() const noexcept -> bool {
            static_assert(IsSafeAutounlink<PackedOptions::LINK_MODE>::VALUE,
                          "is_linked requires SafeLink or AutoUnlink mode.");
            return !Algorithms::unique(this_ptr());
        }

        constexpr auto unlink() noexcept -> void {
            static_assert(PackedOptions::LINK_MODE == LinkModeType::AutoUnlink,
                          "unlink requires AutoUnlink mode.");
            Algorithms::unlink(this_ptr());
        }
    };
} // namespace detail

template <typename... Options>
class AvlSetBaseHook: public detail::AvlSetHookImpl<true, Options...> {
    using Base = detail::AvlSetHookImpl<true, Options...>;

  public:
    using Base::Base;
    using typename Base::ConstNodePtr;
    using typename Base::HookTags;
    using typename Base::NodePtr;
    using typename Base::NodeTraits;
};

template <typename... Options>
class AvlSetMemberHook final: public detail::AvlSetHookImpl<false, Options...> {
    using Base = detail::AvlSetHookImpl<false, Options...>;

  public:
    using Base::Base;
    using typename Base::ConstNodePtr;
    using typename Base::HookTags;
    using typename Base::NodePtr;
    using typename Base::NodeTraits;
};

template <typename... Options>
struct MakeAvlSetBaseHook final {
    using Type = AvlSetBaseHook<Options...>;
};

template <typename... Options>
struct MakeAvlSetMemberHook final {
    using Type = AvlSetMemberHook<Options...>;
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_AVL_SET_HOOK_HPP
