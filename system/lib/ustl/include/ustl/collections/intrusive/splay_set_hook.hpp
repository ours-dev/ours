/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_SPLAY_SET_HOOK_HPP
#define USTL_COLLECTIONS_INTRUSIVE_SPLAY_SET_HOOK_HPP 1

#include <type_traits>

#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/tree_node.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pack_options.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>
#include <ustl/collections/intrusive/splaytree_algorithms.hpp>

namespace ustl::collections::intrusive {
    namespace detail {
        template <bool IsBaseHook, typename... Options>
        class SplaySetHookImpl : public TreeNode<typename PackOptions<HookDefaults, Options...>::Type::VoidPointer> {
            using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;
            using Node = TreeNode<typename PackedOptions::VoidPointer>;
            using Algorithms = SplaytreeAlgorithms<TreeNodeTraits<typename PackedOptions::VoidPointer>>;

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

            constexpr SplaySetHookImpl() noexcept {
                if constexpr (IsSafeAutounlink<PackedOptions::LINK_MODE>::VALUE) {
                    Algorithms::init(this_ptr());
                }
            }

            constexpr SplaySetHookImpl(SplaySetHookImpl const &) noexcept
                : SplaySetHookImpl() {}

            constexpr auto operator=(SplaySetHookImpl const &) noexcept -> SplaySetHookImpl & { return *this; }

            constexpr ~SplaySetHookImpl() noexcept {
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

            constexpr auto swap_nodes(SplaySetHookImpl &other) noexcept -> void {
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
    class SplaySetBaseHook : public detail::SplaySetHookImpl<true, Options...> {
        using Base = detail::SplaySetHookImpl<true, Options...>;

    public:
        using Base::Base;
        using typename Base::ConstNodePtr;
        using typename Base::HookTags;
        using typename Base::NodePtr;
        using typename Base::NodeTraits;
    };

    template <typename... Options>
    class SplaySetMemberHook final : public detail::SplaySetHookImpl<false, Options...> {
        using Base = detail::SplaySetHookImpl<false, Options...>;

    public:
        using Base::Base;
        using typename Base::ConstNodePtr;
        using typename Base::HookTags;
        using typename Base::NodePtr;
        using typename Base::NodeTraits;
    };
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_SPLAY_SET_HOOK_HPP
