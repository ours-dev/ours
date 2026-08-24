/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_SET_HOOK_HPP
#define USTL_COLLECTIONS_INTRUSIVE_SET_HOOK_HPP 1

#include <type_traits>

#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/rbtree_node.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pack_options.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>
#include <ustl/collections/intrusive/rbtree_algorithms.hpp>

namespace ustl::collections::intrusive {
    namespace detail {
        template <bool IsBaseHook, typename... Options>
        class SetHookImpl
            : public RBTreeNodeType<typename PackOptions<HookDefaults, Options...>::Type::VoidPointer,
                                    PackOptions<HookDefaults, Options...>::Type::OPTIMIZE_SIZE> {
            using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;
            using Node = RBTreeNodeType<typename PackedOptions::VoidPointer, PackedOptions::OPTIMIZE_SIZE>;
            using Algorithms = RBTreeAlgorithms<RBTreeNodeTraits<typename PackedOptions::VoidPointer,
                                                                   PackedOptions::OPTIMIZE_SIZE>>;

        public:
            using NodeTraits = RBTreeNodeTraits<typename PackedOptions::VoidPointer, PackedOptions::OPTIMIZE_SIZE>;
            using NodePtr = typename NodeTraits::NodePtr;
            using ConstNodePtr = typename NodeTraits::ConstNodePtr;

            struct HookTags final {
                using NodeTraits = RBTreeNodeTraits<typename PackedOptions::VoidPointer, PackedOptions::OPTIMIZE_SIZE>;
                using Tag = std::conditional_t<IsBaseHook, typename PackedOptions::Tag, MemberTag>;
                static constexpr LinkModeType LINK_MODE = PackedOptions::LINK_MODE;
                static constexpr bool IS_BASE_HOOK = IsBaseHook;
            };

            constexpr SetHookImpl() noexcept {
                if constexpr (IsSafeAutounlink<PackedOptions::LINK_MODE>::VALUE) {
                    Algorithms::init(this_ptr());
                }
            }

            constexpr SetHookImpl(SetHookImpl const &) noexcept
                : SetHookImpl() {}

            constexpr auto operator=(SetHookImpl const &) noexcept -> SetHookImpl & {
                return *this;
            }

            constexpr ~SetHookImpl() noexcept {
                if constexpr (PackedOptions::LINK_MODE == LinkModeType::SafeLink) {
                    detail::require(!is_linked());
                } else if constexpr (PackedOptions::LINK_MODE == LinkModeType::AutoUnlink) {
                    unlink();
                }
            }

            [[nodiscard]] constexpr auto this_ptr() noexcept -> NodePtr {
                return PointerTraits<NodePtr>::pointer_to(static_cast<Node &>(*this));
            }

            [[nodiscard]] constexpr auto this_ptr() const noexcept -> ConstNodePtr {
                return PointerTraits<ConstNodePtr>::pointer_to(static_cast<Node const &>(*this));
            }

            constexpr auto swap_nodes(SetHookImpl &other) noexcept -> void {
                Algorithms::swap_nodes(this_ptr(), other.this_ptr());
            }

            [[nodiscard]] constexpr auto is_linked() const noexcept -> bool {
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
    class SetBaseHook : public detail::SetHookImpl<true, Options...> {
        using Base = detail::SetHookImpl<true, Options...>;

    public:
        using Base::Base;
        using Base::is_linked;
        using typename Base::ConstNodePtr;
        using typename Base::NodePtr;
        using typename Base::NodeTraits;
        using typename Base::HookTags;
    };

    template <typename... Options>
    class SetMemberHook final : public detail::SetHookImpl<false, Options...> {
        using Base = detail::SetHookImpl<false, Options...>;

    public:
        using Base::Base;
        using typename Base::ConstNodePtr;
        using typename Base::NodePtr;
        using typename Base::NodeTraits;
        using typename Base::HookTags;
    };

    template <typename... Options>
    struct MakeSetBaseHook final {
        using Type = SetBaseHook<Options...>;
    };

    template <typename... Options>
    struct MakeSetMemberHook final {
        using Type = SetMemberHook<Options...>;
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_SET_HOOK_HPP
