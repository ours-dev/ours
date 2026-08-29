/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/bs_set_hook.hpp>
#include <ustl/collections/intrusive/detail/key_node_ptr_compare.hpp>
#include <ustl/collections/intrusive/detail/value_traits_access.hpp>
#include <ustl/collections/intrusive/member_value_traits.hpp>

namespace {
using namespace ustl::collections::intrusive;

struct Key final {
    int m_value{};
};

struct BaseElement final: BsSetBaseHook<> {
    Key m_key{};
};

struct MemberElement final {
    Key m_key{};
    BsSetMemberHook<> m_hook{};
};

struct KeyCompare final {
    [[nodiscard]]
    constexpr auto operator()(Key const &left, Key const &right) const noexcept -> bool {
        return left.m_value < right.m_value;
    }
};

struct IsOddKey final {
    [[nodiscard]]
    constexpr auto operator()(Key const &key) const noexcept -> bool {
        return key.m_value % 2 != 0;
    }
};

struct KeyOfElement final {
    template <typename Element>
    [[nodiscard]]
    constexpr auto operator()(Element const &element) const noexcept -> Key const & {
        return element.m_key;
    }
};

using BaseAccess = detail::ValueTraitsAccess<BaseElement, BsSetBaseHook<>>;
using MemberTraits = Mhtraits<MemberElement, BsSetMemberHook<>, &MemberElement::m_hook>;
using MemberAccess = detail::ValueTraitsAccess<MemberElement, MemberTraits>;
using BaseNodeCompare = detail::KeyNodePtrCompare<KeyCompare, BaseAccess, KeyOfElement>;
using MemberNodeCompare = detail::KeyNodePtrCompare<KeyCompare, MemberAccess, KeyOfElement>;
using BaseNodePredicate = detail::KeyNodePtrCompare<IsOddKey, BaseAccess, KeyOfElement>;
} // namespace

auto main() -> int {
    BaseElement base_one{.m_key = {.m_value = 1}};
    BaseElement base_two{.m_key = {.m_value = 2}};
    MemberElement member_one{.m_key = {.m_value = 1}};
    MemberElement member_two{.m_key = {.m_value = 2}};
    KeyCompare const compare{};
    IsOddKey const is_odd{};
    BaseNodeCompare const base_node_compare{&compare};
    MemberNodeCompare const member_node_compare{&compare};
    BaseNodePredicate const base_node_predicate{&is_odd};

    if (!base_node_predicate(BaseAccess::to_node(base_one)) ||
        !base_node_compare(BaseAccess::to_node(base_one), BaseAccess::to_node(base_two)) ||
        !base_node_compare(BaseAccess::to_node(base_one), Key{.m_value = 2}) ||
        !base_node_compare(Key{.m_value = 1}, BaseAccess::to_node(base_two)) ||
        !base_node_compare(Key{.m_value = 1}, Key{.m_value = 2})) {
        return 1;
    }
    return member_node_compare(MemberAccess::to_node(member_one),
                               MemberAccess::to_node(member_two)) &&
                   member_node_compare(MemberAccess::to_node(member_one), Key{.m_value = 2}) &&
                   member_node_compare(Key{.m_value = 1}, MemberAccess::to_node(member_two)) &&
                   member_node_compare(Key{.m_value = 1}, Key{.m_value = 2})
               ? 0
               : 2;
}
