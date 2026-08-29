/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/detail/value_traits_access.hpp>
#include <ustl/collections/intrusive/list_hook.hpp>
#include <ustl/collections/intrusive/member_value_traits.hpp>

namespace {
using namespace ustl::collections::intrusive;

struct BaseValue final: ListBaseHook<> {
    int m_key{};
};

struct MemberValue final {
    int m_key{};
    ListMemberHook<> m_hook{};
};

struct ExplicitValue final {
    int m_key{};
    ListNode<void *> m_node{};
};

using BaseAccess = detail::ValueTraitsAccess<BaseValue, ListBaseHook<>>;
using MemberTraits = Mhtraits<MemberValue, ListMemberHook<>, &MemberValue::m_hook>;
using MemberAccess = detail::ValueTraitsAccess<MemberValue, MemberTraits>;
using ExplicitTraits = MemberValueTraits<ExplicitValue, ListNodeTraits<void *>,
                                         &ExplicitValue::m_node, LinkModeType::SafeLink>;
using ExplicitAccess = detail::ValueTraitsAccess<ExplicitValue, ExplicitTraits>;
} // namespace

auto main() -> int {
    BaseValue base_value{.m_key = 1};
    MemberValue member_value{.m_key = 2};
    ExplicitValue explicit_value{.m_key = 3};

    const auto base_node = BaseAccess::to_node(base_value);
    const auto member_node = MemberAccess::to_node(member_value);
    const auto explicit_node = ExplicitAccess::to_node(explicit_value);

    if (BaseAccess::to_value(base_node) != &base_value ||
        MemberAccess::to_value(member_node) != &member_value ||
        ExplicitAccess::to_value(explicit_node) != &explicit_value) {
        return 1;
    }

    const auto base_const_node = static_cast<BaseValue const &>(base_value).this_ptr();
    const auto member_const_node =
        MemberTraits::to_node_ptr(static_cast<MemberValue const &>(member_value));
    const auto explicit_const_node =
        ExplicitTraits::to_node_ptr(static_cast<ExplicitValue const &>(explicit_value));
    return BaseAccess::to_value(base_const_node) == &base_value &&
                   MemberAccess::to_value(member_const_node) == &member_value &&
                   ExplicitAccess::to_value(explicit_const_node) == &explicit_value
               ? 0
               : 2;
}
