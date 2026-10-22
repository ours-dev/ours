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

#include <type_traits>

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
    /// The void pointer an any node was made with.
    template <typename NodeType>
    struct AnyNodeVoidPointer;

    template <typename VoidPointer>
    struct AnyNodeVoidPointer<AnyNode<VoidPointer>> final {
        using Type = VoidPointer;
    };

    /// What a container needs to know about an item that keeps an AnyMemberHook
    /// in a member: the value traits the `MemberHook` option already makes for
    /// that member (`Inner`, a Mhtraits), seen through the node traits of the
    /// container the hook is being used in.
    ///
    /// An any hook is one node that any container's algorithms can work on, so
    /// the node, the pointer to it and the way from a value to its node do not
    /// change with the container; only the node traits do, because they say
    /// which fields of the node mean "parent", "next", "hash" and so on. Inner
    /// is final and describes the hook with the generic AnyNodeTraits, which has
    /// none of those, so this forwards the conversions to it and names the
    /// container's node traits in its place.
    template <typename Inner, template <typename> class NodeTraitsTemplate>
    struct AnyMemberValueTraits final {
        using HookType = typename Inner::HookType;
        using VoidPointer = typename AnyNodeVoidPointer<typename Inner::Node>::Type;
        using NodeTraits = NodeTraitsTemplate<VoidPointer>;
        using Node = typename NodeTraits::Node;
        using NodePtr = typename NodeTraits::NodePtr;
        using ConstNodePtr = typename NodeTraits::ConstNodePtr;
        using Element = typename Inner::Element;
        using PtrMut = typename Inner::PtrMut;
        using Ptr = typename Inner::Ptr;
        using RefMut = typename Inner::RefMut;
        using Ref = typename Inner::Ref;
        static constexpr bool IS_ANY_HOOK = true;
        static constexpr LinkModeType LINK_MODE = Inner::LINK_MODE;

        struct HookTags final {
            static constexpr bool IS_BASE_HOOK = false;
            static constexpr LinkModeType LINK_MODE = Inner::LINK_MODE;
        };

        static_assert(std::is_same_v<Node, typename Inner::Node> &&
                          std::is_same_v<NodePtr, typename Inner::NodePtr> &&
                          std::is_same_v<ConstNodePtr, typename Inner::ConstNodePtr>,
                      "an any hook's node is the same whatever container it is used in");

        [[nodiscard]]
        static constexpr auto to_node_ptr(RefMut value) noexcept -> NodePtr {
            return Inner::to_node_ptr(value);
        }

        [[nodiscard]]
        static constexpr auto to_node_ptr(Ref value) noexcept -> ConstNodePtr {
            return Inner::to_node_ptr(value);
        }

        [[nodiscard]]
        static auto to_value_ptr(NodePtr const node) noexcept -> PtrMut {
            return Inner::to_value_ptr(node);
        }

        [[nodiscard]]
        static auto to_value_ptr(ConstNodePtr const node) noexcept -> Ptr {
            return Inner::to_value_ptr(node);
        }
    };

    /// For an any base hook: the tags and the node traits of the container,
    /// to be filled in by whoever builds the value traits from them. Nothing
    /// in the tree uses it with a container yet.
    template <typename BasicHook, template <typename> class NodeTraitsTemplate>
    struct AnyBaseProtoValueTraits final {
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

    template <typename BasicHook, template <typename> class NodeTraitsTemplate>
    struct AnyProtoValueTraits final {
        using Type = AnyBaseProtoValueTraits<BasicHook, NodeTraitsTemplate>;
    };

    /// A `MemberHook` option: a container can use it as it stands.
    template <typename BasicHook, template <typename> class NodeTraitsTemplate>
        requires requires { typename BasicHook::MemberValueTraits; }
    struct AnyProtoValueTraits<BasicHook, NodeTraitsTemplate> final {
        using Type =
            AnyMemberValueTraits<typename BasicHook::MemberValueTraits, NodeTraitsTemplate>;
    };

    template <typename BasicHook, template <typename> class NodeTraitsTemplate>
    struct AnyToSomeHook final {
        template <typename Base>
        struct Pack: Base {
            using ProtoValueTraits =
                typename AnyProtoValueTraits<BasicHook, NodeTraitsTemplate>::Type;
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
