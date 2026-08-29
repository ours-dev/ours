/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/list.hpp>

namespace {
struct Value final: ustl::collections::intrusive::ListBaseHook<> {
    int m_value{};
};

using List = ustl::collections::intrusive::List<Value>;

struct MemberValue final {
    int m_key{};
    ustl::collections::intrusive::ListMemberHook<> m_hook{};
};

using MemberList = ustl::collections::intrusive::List<
    MemberValue,
    ustl::collections::intrusive::MemberHook<
        MemberValue, ustl::collections::intrusive::ListMemberHook<>, &MemberValue::m_hook>>;
} // namespace

auto main() -> int {
    Value one{.m_value = 1};
    Value two{.m_value = 2};
    List list{};
    list.push_back(one);
    list.push_front(two);
    if (list.size() != 2U || !list.front() || list.front().value().m_value != 2 ||
        list.begin()->m_value != 2) {
        return 1;
    }
    list.erase(two);
    if (list.size() != 1U || !list.front() || list.front().value().m_value != 1 ||
        two.is_linked()) {
        return 2;
    }

    MemberValue member_one{.m_key = 3};
    MemberValue member_two{.m_key = 5};
    MemberList member_list{};
    member_list.push_back(member_one);
    member_list.push_front(member_two);
    member_list.erase(member_one);
    return member_list.size() == 1U && member_list.front() && member_list.front().value().m_key == 5
               ? 0
               : 3;
}
