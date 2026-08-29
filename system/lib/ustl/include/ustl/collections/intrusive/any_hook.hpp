/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_ANY_HOOK_HPP
#define USTL_COLLECTIONS_INTRUSIVE_ANY_HOOK_HPP 1

#include <ustl/collections/intrusive/detail/any_node_and_algorithms.hpp>
#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pack_options.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

namespace ustl::collections::intrusive {
template <typename... Options>
class AnyBaseHook;

template <typename... Options>
class AnyMemberHook;

template <typename... Options>
struct MakeAnyBaseHook final {
    using Type = AnyBaseHook<Options...>;
};

template <typename... Options>
struct MakeAnyMemberHook final {
    using Type = AnyMemberHook<Options...>;
};

template <typename... Options>
class AnyBaseHook
    : public AnyNode<typename PackOptions<HookDefaults, Options...>::Type::VoidPointer> {
    using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;
    using Node = AnyNode<typename PackedOptions::VoidPointer>;
    using Algorithms = AnyAlgorithms<typename PackedOptions::VoidPointer>;

    static_assert(PackedOptions::LINK_MODE != LinkModeType::AutoUnlink,
                  "Any hooks do not support AutoUnlink mode.");

  public:
    using NodeTraits = AnyNodeTraits<typename PackedOptions::VoidPointer>;
    using NodePtr = typename NodeTraits::NodePtr;
    using ConstNodePtr = typename NodeTraits::ConstNodePtr;

    struct HookTags final {
        using NodeTraits = AnyNodeTraits<typename PackedOptions::VoidPointer>;
        using Tag = typename PackedOptions::Tag;
        static constexpr LinkModeType LINK_MODE = PackedOptions::LINK_MODE;
        static constexpr bool IS_BASE_HOOK = true;
    };

    constexpr AnyBaseHook() noexcept {
        if constexpr (PackedOptions::LINK_MODE == LinkModeType::SafeLink) {
            Algorithms::init(this_ptr());
        }
    }

    constexpr AnyBaseHook(AnyBaseHook const &) noexcept : AnyBaseHook() {}

    constexpr auto operator=(AnyBaseHook const &) noexcept -> AnyBaseHook & {
        return *this;
    }

    constexpr ~AnyBaseHook() noexcept {
        if constexpr (PackedOptions::LINK_MODE == LinkModeType::SafeLink) {
            detail::require(!is_linked());
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

    [[nodiscard]]
    constexpr auto is_linked() const noexcept -> bool {
        static_assert(PackedOptions::LINK_MODE == LinkModeType::SafeLink,
                      "is_linked requires SafeLink mode.");
        return !Algorithms::unique(this_ptr());
    }
};

template <typename... Options>
class AnyMemberHook final
    : public AnyNode<typename PackOptions<HookDefaults, Options...>::Type::VoidPointer> {
    using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;
    using Node = AnyNode<typename PackedOptions::VoidPointer>;
    using Algorithms = AnyAlgorithms<typename PackedOptions::VoidPointer>;

    static_assert(PackedOptions::LINK_MODE != LinkModeType::AutoUnlink,
                  "Any hooks do not support AutoUnlink mode.");

  public:
    using NodeTraits = AnyNodeTraits<typename PackedOptions::VoidPointer>;
    using NodePtr = typename NodeTraits::NodePtr;
    using ConstNodePtr = typename NodeTraits::ConstNodePtr;

    struct HookTags final {
        using NodeTraits = AnyNodeTraits<typename PackedOptions::VoidPointer>;
        using Tag = MemberTag;
        static constexpr LinkModeType LINK_MODE = PackedOptions::LINK_MODE;
        static constexpr bool IS_BASE_HOOK = false;
    };

    constexpr AnyMemberHook() noexcept {
        if constexpr (PackedOptions::LINK_MODE == LinkModeType::SafeLink) {
            Algorithms::init(this_ptr());
        }
    }

    constexpr AnyMemberHook(AnyMemberHook const &) noexcept : AnyMemberHook() {}

    constexpr auto operator=(AnyMemberHook const &) noexcept -> AnyMemberHook & {
        return *this;
    }

    constexpr ~AnyMemberHook() noexcept {
        if constexpr (PackedOptions::LINK_MODE == LinkModeType::SafeLink) {
            detail::require(!is_linked());
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

    [[nodiscard]]
    constexpr auto is_linked() const noexcept -> bool {
        static_assert(PackedOptions::LINK_MODE == LinkModeType::SafeLink,
                      "is_linked requires SafeLink mode.");
        return !Algorithms::unique(this_ptr());
    }
};

namespace detail {
    template <typename BasicHook, template <typename> class NodeTraitsTemplate>
    struct AnyToSomeHook final {
        template <typename Base>
        struct Pack: Base {
            struct ProtoValueTraits final {
                struct HookTags final {
                    static constexpr bool IS_BASE_HOOK = BasicHook::HookTags::IS_BASE_HOOK;
                };

                using BasicHookType = BasicHook;
                static constexpr bool IS_ANY_HOOK = true;

                template <typename VoidPointer>
                struct NodeTraitsFromVoidPointer final {
                    using Type = NodeTraitsTemplate<VoidPointer>;
                };
            };
        };
    };
} // namespace detail

template <typename BasicHook>
using AnyToSlistHook = detail::AnyToSomeHook<BasicHook, AnySlistNodeTraits>;

template <typename BasicHook>
using AnyToListHook = detail::AnyToSomeHook<BasicHook, AnyListNodeTraits>;

template <typename BasicHook>
using AnyToSetHook = detail::AnyToSomeHook<BasicHook, AnyRbtreeNodeTraits>;

template <typename BasicHook>
using AnyToAvlSetHook = detail::AnyToSomeHook<BasicHook, AnyAvltreeNodeTraits>;

template <typename BasicHook>
using AnyToBsSetHook = detail::AnyToSomeHook<BasicHook, AnyTreeNodeTraits>;

template <typename BasicHook>
using AnyToUnorderedSetHook = detail::AnyToSomeHook<BasicHook, AnyUnorderedNodeTraits>;
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_ANY_HOOK_HPP
