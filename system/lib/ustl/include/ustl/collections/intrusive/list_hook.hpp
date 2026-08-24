/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_LIST_HOOK_HPP
#define USTL_COLLECTIONS_INTRUSIVE_LIST_HOOK_HPP 1

#include <ustl/collections/intrusive/circular_list_algorithms.hpp>
#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/list_node.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pack_options.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

namespace ustl::collections::intrusive {
    template <typename... Options>
    class ListBaseHook;

    template <typename... Options>
    class ListMemberHook;

    template <typename... Options>
    struct MakeListBaseHook final {
        using Type = ListBaseHook<Options...>;
    };

    template <typename... Options>
    struct MakeListMemberHook final {
        using Type = ListMemberHook<Options...>;
    };

    template <typename... Options>
    class ListBaseHook
        : public ListNode<typename PackOptions<HookDefaults, Options...>::Type::VoidPointer> {
        using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;
        using Node = ListNode<typename PackedOptions::VoidPointer>;
        using Algorithms = CircularListAlgorithms<ListNodeTraits<typename PackedOptions::VoidPointer>>;

    public:
        using NodeTraits = ListNodeTraits<typename PackedOptions::VoidPointer>;
        using NodePtr = typename NodeTraits::NodePtr;
        using ConstNodePtr = typename NodeTraits::ConstNodePtr;

        struct HookTags final {
            using NodeTraits = ListNodeTraits<typename PackedOptions::VoidPointer>;
            using Tag = typename PackedOptions::Tag;
            static constexpr LinkModeType LINK_MODE = PackedOptions::LINK_MODE;
            static constexpr bool IS_BASE_HOOK = true;
        };

        constexpr ListBaseHook() noexcept {
            if constexpr (IsSafeAutounlink<PackedOptions::LINK_MODE>::VALUE) {
                Algorithms::init(this_ptr());
            }
        }

        constexpr ListBaseHook(ListBaseHook const &) noexcept
            : ListBaseHook() {}

        constexpr auto operator=(ListBaseHook const &) noexcept -> ListBaseHook & {
            return *this;
        }

        constexpr ~ListBaseHook() noexcept {
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

        constexpr auto swap_nodes(ListBaseHook &other) noexcept -> void {
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
            const auto node = this_ptr();
            if (!Algorithms::inited(node)) {
                static_cast<void>(Algorithms::unlink(node));
                Algorithms::init(node);
            }
        }
    };

    template <typename... Options>
    class ListMemberHook final
        : public ListNode<typename PackOptions<HookDefaults, Options...>::Type::VoidPointer> {
        using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;
        using Node = ListNode<typename PackedOptions::VoidPointer>;
        using Algorithms = CircularListAlgorithms<ListNodeTraits<typename PackedOptions::VoidPointer>>;

    public:
        using NodeTraits = ListNodeTraits<typename PackedOptions::VoidPointer>;
        using NodePtr = typename NodeTraits::NodePtr;
        using ConstNodePtr = typename NodeTraits::ConstNodePtr;

        struct HookTags final {
            using NodeTraits = ListNodeTraits<typename PackedOptions::VoidPointer>;
            using Tag = MemberTag;
            static constexpr LinkModeType LINK_MODE = PackedOptions::LINK_MODE;
            static constexpr bool IS_BASE_HOOK = false;
        };

        constexpr ListMemberHook() noexcept {
            if constexpr (IsSafeAutounlink<PackedOptions::LINK_MODE>::VALUE) {
                Algorithms::init(this_ptr());
            }
        }

        constexpr ListMemberHook(ListMemberHook const &) noexcept
            : ListMemberHook() {}

        constexpr auto operator=(ListMemberHook const &) noexcept -> ListMemberHook & {
            return *this;
        }

        constexpr ~ListMemberHook() noexcept {
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

        constexpr auto swap_nodes(ListMemberHook &other) noexcept -> void {
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
            const auto node = this_ptr();
            if (!Algorithms::inited(node)) {
                static_cast<void>(Algorithms::unlink(node));
                Algorithms::init(node);
            }
        }
    };
} // namespace ustl::collections::intrusive

#ifndef USTL_DECLARE_HOOK_OPTION
#define USTL_DECLARE_HOOK_OPTION(OWNER, MEMBER, ALIAS) \
    using ALIAS = ::ustl::collections::intrusive::MemberHook<OWNER, decltype(OWNER::MEMBER), &OWNER::MEMBER>
#endif

#endif // USTL_COLLECTIONS_INTRUSIVE_LIST_HOOK_HPP
