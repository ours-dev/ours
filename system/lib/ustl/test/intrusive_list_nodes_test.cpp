/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <concepts>

#include <ustl/collections/intrusive/detail/list_node.hpp>
#include <ustl/collections/intrusive/detail/slist_node.hpp>

using namespace ustl::collections::intrusive;

using ListTraits = ListNodeTraits<void *>;
using SlistTraits = SlistNodeTraits<void *>;

static_assert(std::same_as<ListTraits::NodePtr, ListNode<void *> *>);
static_assert(std::same_as<ListTraits::ConstNodePtr, const ListNode<void *> *>);
static_assert(std::same_as<SlistTraits::NodePtr, SlistNode<void *> *>);
static_assert(std::same_as<SlistTraits::ConstNodePtr, const SlistNode<void *> *>);

auto main() -> int {
    ListNode<void *> first{};
    ListNode<void *> second{};
    ListTraits::set_next(&first, &second);
    ListTraits::set_previous(&second, &first);
    if (ListTraits::get_next(&first) != &second || ListTraits::get_previous(&second) != &first) {
        return 1;
    }

    const auto *const const_second = &second;
    if (ListTraits::get_previous(const_second) != &first) {
        return 2;
    }

    SlistNode<void *> head{};
    SlistNode<void *> tail{};
    SlistTraits::set_next(&head, &tail);
    if (SlistTraits::get_next(&head) != &tail) {
        return 3;
    }

    const auto *const const_head = &head;
    return SlistTraits::get_next(const_head) == &tail ? 0 : 4;
}
