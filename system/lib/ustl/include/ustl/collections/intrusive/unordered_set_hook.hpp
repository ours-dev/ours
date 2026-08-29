/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_UNORDERED_SET_HOOK_HPP
#define USTL_COLLECTIONS_INTRUSIVE_UNORDERED_SET_HOOK_HPP 1

#include <ustl/collections/intrusive/detail/assert.hpp>
#include <ustl/collections/intrusive/detail/hashtable_node.hpp>
#include <ustl/collections/intrusive/link_mode.hpp>
#include <ustl/collections/intrusive/options.hpp>
#include <ustl/collections/intrusive/pack_options.hpp>
#include <ustl/collections/intrusive/pointer_traits.hpp>

#include <type_traits>

namespace ustl::collections::intrusive {
namespace detail {
    template <bool IsBaseHook, typename... Options>
    class UnorderedSetHookImpl
        : public HashNode<typename PackOptions<HookDefaults, Options...>::Type::VoidPointer,
                          PackOptions<HookDefaults, Options...>::Type::STORE_HASH,
                          PackOptions<HookDefaults, Options...>::Type::OPTIMIZE_MULTIKEY> {
        using PackedOptions = typename PackOptions<HookDefaults, Options...>::Type;
        using Node = HashNode<typename PackedOptions::VoidPointer, PackedOptions::STORE_HASH,
                              PackedOptions::OPTIMIZE_MULTIKEY>;

      public:
        using NodeTraits =
            HashNodeTraits<typename PackedOptions::VoidPointer, PackedOptions::STORE_HASH,
                           PackedOptions::OPTIMIZE_MULTIKEY>;
        using NodePtr = typename NodeTraits::NodePtr;
        using ConstNodePtr = typename NodeTraits::ConstNodePtr;

        struct HookTags final {
            using NodeTraits = typename UnorderedSetHookImpl::NodeTraits;
            using Tag = std::conditional_t<IsBaseHook, typename PackedOptions::Tag, MemberTag>;
            static constexpr LinkModeType LINK_MODE = PackedOptions::LINK_MODE;
            static constexpr bool IS_BASE_HOOK = IsBaseHook;
        };

        constexpr UnorderedSetHookImpl() noexcept {
            NodeTraits::init(this_ptr());
        }

        constexpr UnorderedSetHookImpl(UnorderedSetHookImpl const &) noexcept
            : UnorderedSetHookImpl() {}

        constexpr auto operator=(UnorderedSetHookImpl const &) noexcept -> UnorderedSetHookImpl & {
            return *this;
        }

        constexpr ~UnorderedSetHookImpl() noexcept {
            if constexpr (PackedOptions::LINK_MODE != LinkModeType::NormalLink) {
                detail::require(!NodeTraits::is_linked(this_ptr()));
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
            static_assert(PackedOptions::LINK_MODE != LinkModeType::NormalLink,
                          "is_linked requires SafeLink or AutoUnlink mode.");
            return NodeTraits::is_linked(this_ptr());
        }
    };
} // namespace detail

template <typename... Options>
class UnorderedSetBaseHook: public detail::UnorderedSetHookImpl<true, Options...> {
    using Base = detail::UnorderedSetHookImpl<true, Options...>;

  public:
    using Base::Base;
    using typename Base::ConstNodePtr;
    using typename Base::HookTags;
    using typename Base::NodePtr;
    using typename Base::NodeTraits;
};

template <typename... Options>
class UnorderedSetMemberHook final: public detail::UnorderedSetHookImpl<false, Options...> {
    using Base = detail::UnorderedSetHookImpl<false, Options...>;

  public:
    using Base::Base;
    using typename Base::ConstNodePtr;
    using typename Base::HookTags;
    using typename Base::NodePtr;
    using typename Base::NodeTraits;
};

template <typename... Options>
struct MakeUnorderedSetBaseHook final {
    using Type = UnorderedSetBaseHook<Options...>;
};

template <typename... Options>
struct MakeUnorderedSetMemberHook final {
    using Type = UnorderedSetMemberHook<Options...>;
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_UNORDERED_SET_HOOK_HPP
