/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// An any hook kept in a member works with a container: here a Set, then a List,
// then a singly linked list, with the same hook of the same item one after the
// other. That is what an any
// hook is for: the item says once that it can be linked, and which container
// it is linked into is chosen where the container is declared.

#include <ustl/collections/intrusive/any_hook.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/collections/intrusive/set.hpp>
#include <ustl/collections/intrusive/slist.hpp>

namespace {
namespace in = ustl::collections::intrusive;

struct Item final {
    int m_key{};
    in::AnyMemberHook<> m_hook{};
};

struct Compare final {
    [[nodiscard]]
    constexpr auto operator()(Item const &left, Item const &right) const noexcept -> bool {
        return left.m_key < right.m_key;
    }
};

using ItemHook = in::MemberHook<Item, in::AnyMemberHook<>, &Item::m_hook>;
using Items = in::Set<Item, in::AnyToSetHook<ItemHook>, in::Compare<Compare>>;
using Queue = in::List<Item, in::AnyToListHook<ItemHook>>;
using Parked = in::Slist<Item, in::AnyToSlistHook<ItemHook>>;
} // namespace

auto main() -> int {
    Item one{.m_key = 1};
    Item two{.m_key = 2};
    Item three{.m_key = 3};
    Item duplicate{.m_key = 2};

    if (one.m_hook.is_linked()) {
        return 1;
    }

    // Inserted out of order, read back in order.
    Items items{};
    auto const [two_it, inserted_two] = items.insert(two);
    auto const [three_it, inserted_three] = items.insert(three);
    auto const [one_it, inserted_one] = items.insert(one);
    auto const [duplicate_it, inserted_duplicate] = items.insert(duplicate);
    if (!inserted_one || !inserted_two || !inserted_three || inserted_duplicate ||
        &*duplicate_it != &two || items.size() != 3U) {
        return 2;
    }
    if (!one.m_hook.is_linked() || !two.m_hook.is_linked() || !three.m_hook.is_linked() ||
        duplicate.m_hook.is_linked()) {
        return 3;
    }

    int expect = 1;
    for (auto const &item: items) {
        if (item.m_key != expect) {
            return 4;
        }
        expect += 1;
    }
    if (expect != 4) {
        return 5;
    }

    // Found, and from an item back to its place.
    auto const found = items.find(two);
    if (!found || &found.value() != &two || &*items.iterator_to(three) != &three) {
        return 6;
    }

    // Erased: unlinked again, the others stay.
    auto _ = items.erase(items.iterator_to(two));
    if (two.m_hook.is_linked() || !one.m_hook.is_linked() || !three.m_hook.is_linked() ||
        items.size() != 2U || items.find(two)) {
        return 7;
    }
    items.erase(one);
    if (one.m_hook.is_linked() || items.size() != 1U) {
        return 8;
    }

    // The same hook in another kind of container, now that the Set has let go
    // of it. (An item is in one container at a time: the hook has one node.)
    Queue queue{};
    queue.push_back(two);
    queue.push_back(one);
    if (!two.m_hook.is_linked() || !one.m_hook.is_linked() || queue.size() != 2U ||
        queue.front().value().m_key != 2) {
        return 9;
    }
    auto _ = queue.erase(queue.begin());
    if (two.m_hook.is_linked() || queue.size() != 1U || queue.front().value().m_key != 1) {
        return 10;
    }

    // Clearing a container unlinks what is in it.
    items.clear();
    queue.clear();
    if (one.m_hook.is_linked() || three.m_hook.is_linked() || !items.empty() || !queue.empty()) {
        return 11;
    }

    // A singly linked list for what has no order to keep. Pushed and popped at
    // the front; an item in the middle is taken out by a walk from the front.
    // Every node reads as linked, the last one too: that is why it is not a
    // linear list, whose last node has the null link of an unlinked one.
    Parked parked{};
    if (!parked.empty()) {
        return 12;
    }
    parked.push_front(one);
    parked.push_front(two);
    parked.push_front(three);
    if (!one.m_hook.is_linked() || !two.m_hook.is_linked() || !three.m_hook.is_linked()) {
        return 13;
    }
    if (parked.size() != 3U) {
        return 18;
    }
    if (parked.front().value().m_key != 3) {
        return 19;
    }
    auto _ = parked.erase(parked.iterator_to(two));
    if (two.m_hook.is_linked() || !one.m_hook.is_linked() || parked.size() != 2U ||
        parked.front().value().m_key != 3) {
        return 14;
    }
    auto const popped = parked.pop_front();
    if (popped.is_none() || &popped.value() != &three || three.m_hook.is_linked() ||
        parked.front().value().m_key != 1) {
        return 15;
    }

    // And into a Set after it, which is the whole point of the hook.
    auto const [three_again, inserted_again] = items.insert(three);
    if (!inserted_again || !three.m_hook.is_linked()) {
        return 16;
    }
    items.clear();
    parked.clear();
    if (one.m_hook.is_linked() || three.m_hook.is_linked() || !parked.empty()) {
        return 17;
    }
    return 0;
}
