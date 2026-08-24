/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/slist.hpp>

namespace {
    struct Value final : ustl::collections::intrusive::SlistBaseHook<> {
        int m_value {};
    };

    using Slist = ustl::collections::intrusive::Slist<Value>;

    struct MemberValue final {
        int m_key {};
        ustl::collections::intrusive::SlistMemberHook<> m_hook {};
    };

    using MemberSlist = ustl::collections::intrusive::Slist<
        MemberValue,
        ustl::collections::intrusive::MemberHook<
            MemberValue,
            ustl::collections::intrusive::SlistMemberHook<>,
            &MemberValue::m_hook>>;
} // namespace

auto main() -> int {
    Value one {.m_value = 1};
    Value two {.m_value = 2};
    Slist list {};
    list.push_front(one);
    list.push_front(two);
    if (list.size() != 2U || !list.front() || list.front().value().m_value != 2 || list.begin()->m_value != 2) {
        return 1;
    }
    const auto removed = list.pop_front();
    if (!removed || &removed.value() != &two || list.size() != 1U || !list.front() || list.front().value().m_value != 1) {
        return 2;
    }
    list.clear();
    if (!list.empty() || one.is_linked() || two.is_linked()) {
        return 3;
    }

    MemberValue member_one {.m_key = 3};
    MemberValue member_two {.m_key = 5};
    MemberSlist member_slist {};
    member_slist.push_front(member_one);
    member_slist.push_front(member_two);
    const auto member_removed = member_slist.pop_front();
    return member_removed && &member_removed.value() == &member_two && member_slist.front() &&
            member_slist.front().value().m_key == 3
        ? 0
        : 4;
}
