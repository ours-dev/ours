/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/set.hpp>

namespace {
struct Value final: ustl::collections::intrusive::SetBaseHook<> {
    int m_key{};
};

struct Compare final {
    [[nodiscard]]
    constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
        return left.m_key < right.m_key;
    }
};

using Set =
    ustl::collections::intrusive::Set<Value, ustl::collections::intrusive::Compare<Compare>>;

struct MemberValue final {
    int m_key{};
    ustl::collections::intrusive::SetMemberHook<> m_hook{};
};

struct MemberCompare final {
    [[nodiscard]]
    constexpr auto operator()(MemberValue const &left, MemberValue const &right) const noexcept
        -> bool {
        return left.m_key < right.m_key;
    }
};

using MemberSet = ustl::collections::intrusive::Set<
    MemberValue,
    ustl::collections::intrusive::MemberHook<
        MemberValue, ustl::collections::intrusive::SetMemberHook<>, &MemberValue::m_hook>,
    ustl::collections::intrusive::Compare<MemberCompare>>;

using Multiset =
    ustl::collections::intrusive::Multiset<Value, ustl::collections::intrusive::Compare<Compare>>;

using LegacyMultiset =
    ustl::collections::intrusive::MultiSet<Value, ustl::collections::intrusive::Compare<Compare>>;

static_assert(std::same_as<Multiset, LegacyMultiset>);
} // namespace

auto main() -> int {
    Value one{.m_key = 1};
    Value two{.m_key = 2};
    Value duplicate{.m_key = 2};
    Set set{};
    const auto [one_it, inserted_one] = set.insert(one);
    const auto [two_it, inserted_two] = set.insert(two);
    const auto [duplicate_it, inserted_duplicate] = set.insert(duplicate);
    if (!inserted_one || !inserted_two || inserted_duplicate || one_it->m_key != 1 ||
        two_it->m_key != 2 || duplicate_it->m_key != 2 || set.size() != 2U) {
        return 1;
    }

    const auto found = set.find(two);
    if (!found || found.value().m_key != 2) {
        return 2;
    }
    set.erase(one);
    if (set.find(one) || set.begin()->m_key != 2 || set.size() != 1U) {
        return 3;
    }

    // Looked up by a bare key: the first element past 1 is 2, and nothing is
    // past 2.
    Value lookup_one{.m_key = 1};
    auto _ = set.insert(lookup_one);
    auto const key_lower =
        set.lower_bound(2, [](Value const &value, int key) noexcept { return value.m_key < key; });
    auto const past_one =
        set.upper_bound(1, [](int key, Value const &value) noexcept { return key < value.m_key; });
    auto const past_two =
        set.upper_bound(2, [](int key, Value const &value) noexcept { return key < value.m_key; });
    if (key_lower == set.end() || key_lower->m_key != 2 || past_one == set.end() ||
        past_one->m_key != 2 || past_two != set.end()) {
        return 8;
    }
    set.erase(lookup_one);

    MemberValue member_one{.m_key = 4};
    MemberValue member_two{.m_key = 6};
    MemberSet member_set{};
    const auto [member_one_it, inserted_member_one] = member_set.insert(member_one);
    const auto [member_two_it, inserted_member_two] = member_set.insert(member_two);
    const auto member_found = member_set.find(member_two);
    if (!inserted_member_one || !inserted_member_two || member_one_it->m_key != 4 ||
        member_two_it->m_key != 6 || !member_found || &member_found.value() != &member_two) {
        return 4;
    }

    Value multi_one{.m_key = 8};
    Value multi_two{.m_key = 8};
    Multiset multiset{};
    const auto multi_one_it = multiset.insert(multi_one);
    const auto multi_two_it = multiset.insert(multi_two);
    const auto multi_found = multiset.find(multi_one);
    if (multi_one_it->m_key != 8 || multi_two_it->m_key != 8 || !multi_found ||
        multiset.count(multi_one) != 2U) {
        return 5;
    }

    Value range_three{.m_key = 3};
    Value range_seven{.m_key = 7};
    Value range_nine{.m_key = 9};
    LegacyMultiset range{};
    auto _ = range.insert(range_three);
    auto _ = range.insert(range_seven);
    auto _ = range.insert(range_nine);

    const auto lower = range.lower_bound(
        6, [](Value const &value, int key) noexcept { return value.m_key < key; });
    const auto upper = range.upper_bound(
        7, [](int key, Value const &value) noexcept { return key < value.m_key; });
    if (lower == range.end() || lower->m_key != 7 || upper == range.end() || upper->m_key != 9) {
        return 6;
    }

    auto _ = range.erase(range.iterator_to(range_seven));
    int disposed = 0;
    auto _ = range.erase_and_dispose(range.begin(), range.end(),
                                     [&disposed](Value *) noexcept { ++disposed; });
    return range.empty() && disposed == 2 ? 0 : 7;
}
