/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/slist.hpp>

#include <utility>

namespace {
struct Value final: ustl::collections::intrusive::SlistBaseHook<> {
    int m_value{};
};

using Slist = ustl::collections::intrusive::Slist<Value>;
using LinearSlist =
    ustl::collections::intrusive::Slist<Value, ustl::collections::intrusive::Linear<true>>;
using CachedLinearSlist =
    ustl::collections::intrusive::Slist<Value, ustl::collections::intrusive::Linear<true>,
                                        ustl::collections::intrusive::CacheLast<true>>;

struct MemberValue final {
    int m_key{};
    ustl::collections::intrusive::SlistMemberHook<> m_hook{};
};

using MemberSlist = ustl::collections::intrusive::Slist<
    MemberValue,
    ustl::collections::intrusive::MemberHook<
        MemberValue, ustl::collections::intrusive::SlistMemberHook<>, &MemberValue::m_hook>>;
} // namespace

auto main() -> int {
    Value one{.m_value = 1};
    Value two{.m_value = 2};
    Slist list{};
    list.push_front(one);
    list.push_front(two);
    if (list.size() != 2U || !list.front() || list.front().value().m_value != 2 ||
        list.begin()->m_value != 2) {
        return 1;
    }
    const auto removed = list.pop_front();
    if (!removed || &removed.value() != &two || list.size() != 1U || !list.front() ||
        list.front().value().m_value != 1) {
        return 2;
    }
    list.clear();
    if (!list.empty() || one.is_linked() || two.is_linked()) {
        return 3;
    }

    MemberValue member_one{.m_key = 3};
    MemberValue member_two{.m_key = 5};
    MemberSlist member_slist{};
    member_slist.push_front(member_one);
    member_slist.push_front(member_two);
    const auto member_removed = member_slist.pop_front();
    if (!member_removed || &member_removed.value() != &member_two || !member_slist.front() ||
        member_slist.front().value().m_key != 3) {
        return 4;
    }

    // A move constructs an empty list and swaps it with the source, and move
    // assignment clears the target before swapping, so every move swaps an
    // empty header with another. The source must come out empty -- its header
    // pointing at itself rather than at the list it traded with -- and still
    // usable; the list that received the elements must end at its own header.
    Slist empty_source{};
    const Slist from_empty(std::move(empty_source));
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what is under test.
    if (!empty_source.empty() || !from_empty.empty()) {
        return 5;
    }

    Value three{.m_value = 3};
    Value four{.m_value = 4};
    Slist source{};
    source.push_front(four);
    source.push_front(three);
    Slist moved(std::move(source));
    auto moved_end = moved.begin();
    ++moved_end;
    ++moved_end;
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what is under test.
    if (!source.empty() || moved.size() != 2U || moved.begin()->m_value != 3 ||
        moved_end != moved.end()) {
        return 6;
    }
    source.push_front(one);
    auto source_end = source.begin();
    ++source_end;
    if (source.size() != 1U || source.begin()->m_value != 1 || source_end != source.end()) {
        return 7;
    }

    Slist assigned{};
    assigned.push_front(two);
    assigned = std::move(moved);
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what is under test.
    if (!moved.empty() || two.is_linked() || assigned.size() != 2U ||
        assigned.begin()->m_value != 3) {
        return 8;
    }

    // swap() the other way round: the empty header is the argument this time.
    Slist emptied{};
    assigned.swap(emptied);
    auto emptied_end = emptied.begin();
    ++emptied_end;
    ++emptied_end;
    if (!assigned.empty() || emptied.size() != 2U || emptied.begin()->m_value != 3 ||
        emptied_end != emptied.end()) {
        return 9;
    }

    // A linear list ends in a null pointer rather than back at its header, so
    // its swap() exchanges the two headers' successors; there is no
    // swap_nodes() for it to call. A move and sort() both go through swap(),
    // which is how either used to fail to compile for a linear list.
    Value five{.m_value = 5};
    Value six{.m_value = 6};
    LinearSlist linear_source{};
    linear_source.push_front(five);
    linear_source.push_front(six);
    LinearSlist linear_moved(std::move(linear_source));
    // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what is under test.
    if (!linear_source.empty() || linear_moved.size() != 2U || linear_moved.begin()->m_value != 6) {
        return 10;
    }
    linear_moved.sort([](Value const &left, Value const &right) noexcept {
        return left.m_value < right.m_value;
    });
    auto linear_end = linear_moved.begin();
    ++linear_end;
    ++linear_end;
    if (linear_moved.begin()->m_value != 5 || linear_end != linear_moved.end()) {
        return 11;
    }

    // With the last node cached, swap() has to leave each list caching its
    // own last node, or the next push_back() appends to the other list.
    Value seven{.m_value = 7};
    Value eight{.m_value = 8};
    Value nine{.m_value = 9};
    CachedLinearSlist cached{};
    cached.push_back(seven);
    CachedLinearSlist cached_other{};
    cached.swap(cached_other);
    cached_other.push_back(eight);
    cached.push_back(nine);
    auto cached_other_second = cached_other.begin();
    ++cached_other_second;
    if (cached.size() != 1U || cached.begin()->m_value != 9 || cached_other.size() != 2U ||
        cached_other.begin()->m_value != 7 || cached_other_second->m_value != 8) {
        return 12;
    }

    // In a linear list the last element's next is null, just like an element
    // in no list at all, so iterator_to() must not use a null next to reject
    // unlinked elements there; it once did, and trapped on every last element.
    // `six` is last in the sorted `linear_moved`.
    const auto six_position = linear_moved.iterator_to(six);
    if (six_position->m_value != 6 || linear_moved.previous(six_position) != linear_moved.begin() ||
        std::as_const(linear_moved).iterator_to(six)->m_value != 6) {
        return 13;
    }
    const auto after_six = linear_moved.erase(six_position);
    if (after_six != linear_moved.end() || linear_moved.size() != 1U ||
        linear_moved.begin()->m_value != 5) {
        return 14;
    }
    return 0;
}
