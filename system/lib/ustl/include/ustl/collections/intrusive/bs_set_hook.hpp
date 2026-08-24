/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_BS_SET_HOOK_HPP
#define USTL_COLLECTIONS_INTRUSIVE_BS_SET_HOOK_HPP 1

#include <type_traits>

#include <ustl/collections/intrusive/bstree_algorithms.hpp>
#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/tree_node.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pack_options.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

namespace ustl::collections::intrusive {
    namespace detail {
        template <bool IsBaseHook, typename... Options>
        class BsSetHookImpl : public TreeNode<typename PackOptions<HookDefaults, Options...>::Type::VoidPointer> {
            using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;
            using Node = TreeNode<typename PackedOptions::VoidPointer>;
            using Algorithms = BstreeAlgorithms<TreeNodeTraits<typename PackedOptions::VoidPointer>>;

        public:
            using NodeTraits = TreeNodeTraits<typename PackedOptions::VoidPointer>;
            using NodePtr = typename NodeTraits::NodePtr;
            using ConstNodePtr = typename NodeTraits::ConstNodePtr;

            struct HookTags final {
                using NodeTraits = TreeNodeTraits<typename PackedOptions::VoidPointer>;
                using Tag = std::conditional_t<IsBaseHook, typename PackedOptions::Tag, MemberTag>;
                static constexpr LinkModeType LINK_MODE = PackedOptions::LINK_MODE;
                static constexpr bool IS_BASE_HOOK = IsBaseHook;
            };

            constexpr BsSetHookImpl() noexcept {
                if constexpr (IsSafeAutounlink<PackedOptions::LINK_MODE>::VALUE) {
                    Algorithms::init(this_ptr());
                }
            }

            constexpr BsSetHookImpl(BsSetHookImpl const &) noexcept
                : BsSetHookImpl() {}

            constexpr auto operator=(BsSetHookImpl const &) noexcept -> BsSetHookImpl & {
                return *this;
            }

            constexpr ~BsSetHookImpl() noexcept {
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

            constexpr auto swap_nodes(BsSetHookImpl &other) noexcept -> void {
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
    class BsSetBaseHook : public detail::BsSetHookImpl<true, Options...> {
        using Base = detail::BsSetHookImpl<true, Options...>;

    public:
        using Base::Base;
        using typename Base::ConstNodePtr;
        using typename Base::HookTags;
        using typename Base::NodePtr;
        using typename Base::NodeTraits;
    };

    template <typename... Options>
    class BsSetMemberHook final : public detail::BsSetHookImpl<false, Options...> {
        using Base = detail::BsSetHookImpl<false, Options...>;

    public:
        using Base::Base;
        using typename Base::ConstNodePtr;
        using typename Base::HookTags;
        using typename Base::NodePtr;
        using typename Base::NodeTraits;
    };

    template <typename... Options>
    struct MakeBsSetBaseHook final {
        using Type = BsSetBaseHook<Options...>;
    };

    template <typename... Options>
    struct MakeBsSetMemberHook final {
        using Type = BsSetMemberHook<Options...>;
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_BS_SET_HOOK_HPP
