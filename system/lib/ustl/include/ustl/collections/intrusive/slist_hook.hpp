/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_SLIST_HOOK_HPP
#define USTL_COLLECTIONS_INTRUSIVE_SLIST_HOOK_HPP 1

#include <ustl/collections/intrusive/circular_slist_algorithms.hpp>
#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/slist_node.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pack_options.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

namespace ustl::collections::intrusive {
template <typename... Options>
class SlistBaseHook;

template <typename... Options>
class SlistMemberHook;

template <typename... Options>
struct MakeSlistBaseHook final {
    using Type = SlistBaseHook<Options...>;
};

template <typename... Options>
struct MakeSlistMemberHook final {
    using Type = SlistMemberHook<Options...>;
};

template <typename... Options>
class SlistBaseHook
    : public SlistNode<typename PackOptions<HookDefaults, Options...>::Type::VoidPointer> {
    using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;
    using Node = SlistNode<typename PackedOptions::VoidPointer>;
    using Algorithms =
        CircularSlistAlgorithms<SlistNodeTraits<typename PackedOptions::VoidPointer>>;

  public:
    using NodeTraits = SlistNodeTraits<typename PackedOptions::VoidPointer>;
    using NodePtr = typename NodeTraits::NodePtr;
    using ConstNodePtr = typename NodeTraits::ConstNodePtr;

    struct HookTags final {
        using NodeTraits = SlistNodeTraits<typename PackedOptions::VoidPointer>;
        using Tag = typename PackedOptions::Tag;
        static constexpr LinkModeType LINK_MODE = PackedOptions::LINK_MODE;
        static constexpr bool IS_BASE_HOOK = true;
    };

    constexpr SlistBaseHook() noexcept {
        if constexpr (IsSafeAutounlink<PackedOptions::LINK_MODE>::VALUE) {
            Algorithms::init(this_ptr());
        }
    }

    constexpr SlistBaseHook(SlistBaseHook const &) noexcept : SlistBaseHook() {}

    constexpr auto operator=(SlistBaseHook const &) noexcept -> SlistBaseHook & {
        return *this;
    }

    constexpr ~SlistBaseHook() noexcept {
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

    constexpr auto swap_nodes(SlistBaseHook &other) noexcept -> void {
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
        const auto node = this_ptr();
        if (!Algorithms::inited(node)) {
            Algorithms::unlink(node);
            Algorithms::init(node);
        }
    }
};

template <typename... Options>
class SlistMemberHook final
    : public SlistNode<typename PackOptions<HookDefaults, Options...>::Type::VoidPointer> {
    using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;
    using Node = SlistNode<typename PackedOptions::VoidPointer>;
    using Algorithms =
        CircularSlistAlgorithms<SlistNodeTraits<typename PackedOptions::VoidPointer>>;

  public:
    using NodeTraits = SlistNodeTraits<typename PackedOptions::VoidPointer>;
    using NodePtr = typename NodeTraits::NodePtr;
    using ConstNodePtr = typename NodeTraits::ConstNodePtr;

    struct HookTags final {
        using NodeTraits = SlistNodeTraits<typename PackedOptions::VoidPointer>;
        using Tag = MemberTag;
        static constexpr LinkModeType LINK_MODE = PackedOptions::LINK_MODE;
        static constexpr bool IS_BASE_HOOK = false;
    };

    constexpr SlistMemberHook() noexcept {
        if constexpr (IsSafeAutounlink<PackedOptions::LINK_MODE>::VALUE) {
            Algorithms::init(this_ptr());
        }
    }

    constexpr SlistMemberHook(SlistMemberHook const &) noexcept : SlistMemberHook() {}

    constexpr auto operator=(SlistMemberHook const &) noexcept -> SlistMemberHook & {
        return *this;
    }

    constexpr ~SlistMemberHook() noexcept {
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

    constexpr auto swap_nodes(SlistMemberHook &other) noexcept -> void {
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
        const auto node = this_ptr();
        if (!Algorithms::inited(node)) {
            Algorithms::unlink(node);
            Algorithms::init(node);
        }
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_SLIST_HOOK_HPP
