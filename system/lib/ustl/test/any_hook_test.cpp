/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <concepts>
#include <cstddef>

#include <ustl/collections/intrusive/any_hook.hpp>
#include <ustl/collections/intrusive/detail/any_node_and_algorithms.hpp>

namespace {
    using Node = ustl::collections::intrusive::AnyNode<void *>;
    using Algorithms = ustl::collections::intrusive::AnyAlgorithms<void *>;
    using ListTraits = ustl::collections::intrusive::AnyListNodeTraits<void *>;
    using SlistTraits = ustl::collections::intrusive::AnySlistNodeTraits<void *>;
    using UnorderedTraits = ustl::collections::intrusive::AnyUnorderedNodeTraits<void *>;
    using RBTreeTraits = ustl::collections::intrusive::AnyRbtreeNodeTraits<void *>;
    using AvltreeTraits = ustl::collections::intrusive::AnyAvltreeNodeTraits<void *>;
    using TreeTraits = ustl::collections::intrusive::AnyTreeNodeTraits<void *>;
    using BaseHook = ustl::collections::intrusive::AnyBaseHook<>;
    using MemberHook = ustl::collections::intrusive::AnyMemberHook<>;

    using AnyListOption = ustl::collections::intrusive::AnyToListHook<BaseHook>;
    using AnyListValueTraits = AnyListOption::Pack<ustl::collections::intrusive::Empty>::ProtoValueTraits;

    static_assert(AnyListValueTraits::IS_ANY_HOOK);
    static_assert(AnyListValueTraits::HookTags::IS_BASE_HOOK);
    static_assert(std::same_as<typename AnyListValueTraits::template NodeTraitsFromVoidPointer<void *>::Type,
                               ListTraits>);
} // namespace

auto main() -> int {
    Node first{};
    Node second{};
    Algorithms::init(&first);
    Algorithms::init(&second);
    if (!Algorithms::inited(&first) || !Algorithms::unique(&second)) {
        return 1;
    }

    ListTraits::set_next(&first, &second);
    ListTraits::set_previous(&first, &second);
    if (ListTraits::get_next(&first) != &second || ListTraits::get_previous(&first) != &second) {
        return 2;
    }

    SlistTraits::set_next(&first, nullptr);
    UnorderedTraits::set_prev_in_group(&first, &second);
    UnorderedTraits::set_hash(&first, 42U);
    if (SlistTraits::get_next(&first) != nullptr || UnorderedTraits::get_prev_in_group(&first) != &second ||
        UnorderedTraits::get_hash(&first) != 42U) {
        return 3;
    }

    RBTreeTraits::set_parent(&first, &second);
    RBTreeTraits::set_left(&first, nullptr);
    RBTreeTraits::set_right(&first, &second);
    RBTreeTraits::set_color(&first, RBTreeTraits::red());
    if (RBTreeTraits::get_parent(&first) != &second || RBTreeTraits::get_left(&first) != nullptr ||
        RBTreeTraits::get_right(&first) != &second || RBTreeTraits::get_color(&first) != RBTreeTraits::red()) {
        return 4;
    }

    AvltreeTraits::set_balance(&first, AvltreeTraits::positive());
    TreeTraits::set_parent(&first, nullptr);
    TreeTraits::set_left(&first, &second);
    TreeTraits::set_right(&first, nullptr);
    if (AvltreeTraits::get_balance(&first) != AvltreeTraits::positive() || TreeTraits::get_parent(&first) != nullptr ||
        TreeTraits::get_left(&first) != &second || TreeTraits::get_right(&first) != nullptr) {
        return 5;
    }

    BaseHook base{};
    BaseHook copied(base);
    MemberHook member{};
    return !base.is_linked() && !copied.is_linked() && !member.is_linked() ? 0 : 6;
}
