/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/detail/iiterator.hpp>
#include <ustl/collections/intrusive/detail/slist_node.hpp>

#include <concepts>
#include <iterator>

namespace {
struct Item final: ustl::collections::intrusive::SlistNode<void *> {
    int m_value{};
};

struct StaticValueTraits final {
    using NodeTraits = ustl::collections::intrusive::SlistNodeTraits<void *>;
    using Node = typename NodeTraits::Node;
    using NodePtr = typename NodeTraits::NodePtr;
    using Element = Item;
    using PtrMut = Item *;
    using Ptr = Item const *;

    static constexpr bool IS_STATEFUL_VALUE_TRAITS = false;

    [[nodiscard]]
    static constexpr auto to_value_ptr(NodePtr const node) noexcept -> PtrMut {
        return static_cast<PtrMut>(node);
    }
};

struct StatefulValueTraits final {
    using NodeTraits = ustl::collections::intrusive::SlistNodeTraits<void *>;
    using Node = typename NodeTraits::Node;
    using NodePtr = typename NodeTraits::NodePtr;
    using Element = Item;
    using PtrMut = Item *;
    using Ptr = Item const *;
    using ValueTraitsPtr = StatefulValueTraits *;

    static constexpr bool IS_STATEFUL_VALUE_TRAITS = true;

    [[nodiscard]]
    constexpr auto to_value_ptr(NodePtr const node) const noexcept -> PtrMut {
        return static_cast<PtrMut>(node);
    }
};

using Types = ustl::collections::intrusive::detail::Iiter<StaticValueTraits, false,
                                                          std::forward_iterator_tag>;
using Members = ustl::collections::intrusive::detail::IiterMembers<
    typename Types::NodePtr, typename Types::ConstValueTraitsPtr, Types::IS_STATEFUL_VALUE_TRAITS>;
using StatefulTypes = ustl::collections::intrusive::detail::Iiter<StatefulValueTraits, true,
                                                                  std::forward_iterator_tag>;
using StatefulMembers =
    ustl::collections::intrusive::detail::IiterMembers<typename StatefulTypes::NodePtr,
                                                       typename StatefulTypes::ConstValueTraitsPtr,
                                                       StatefulTypes::IS_STATEFUL_VALUE_TRAITS>;

static_assert(std::same_as<typename Types::Element, Item>);
static_assert(std::same_as<typename Types::PtrMut, Item *>);
static_assert(std::same_as<typename Types::RefMut, Item &>);
static_assert(std::same_as<typename Types::DifferenceType, std::ptrdiff_t>);
static_assert(!Types::IS_STATEFUL_VALUE_TRAITS);
static_assert(std::same_as<typename StatefulTypes::Ptr, Item const *>);
static_assert(StatefulTypes::IS_STATEFUL_VALUE_TRAITS);
} // namespace

auto main() -> int {
    Item item{};
    Members members(&item, nullptr);
    StatefulValueTraits traits{};
    StatefulMembers stateful_members(&item, &traits);
    return members.m_node_ptr == &item && members.get_value_traits() == nullptr &&
                   stateful_members.m_node_ptr == &item &&
                   stateful_members.get_value_traits() == &traits
               ? 0
               : 1;
}
