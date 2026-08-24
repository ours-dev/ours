/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/derivation_value_traits.hpp>
#include <ustl/collections/intrusive/list_hook.hpp>
#include <ustl/collections/intrusive/member_value_traits.hpp>
#include <ustl/collections/intrusive/trivial_value_traits.hpp>

namespace {
    struct Value final {
        int m_prefix {};
        ustl::collections::intrusive::ListMemberHook<> m_hook {};
        int m_payload {};
    };

    using Traits = ustl::collections::intrusive::Mhtraits<
        Value,
        ustl::collections::intrusive::ListMemberHook<>,
        &Value::m_hook>;

    struct DirectValue final {
        int m_prefix {};
        ustl::collections::intrusive::ListNode<void *> m_node {};
        int m_payload {};
    };

    using DirectTraits = ustl::collections::intrusive::MemberValueTraits<
        DirectValue,
        ustl::collections::intrusive::ListNodeTraits<void *>,
        &DirectValue::m_node>;

    struct DerivedValue final : ustl::collections::intrusive::ListNode<void *> {
        int m_payload {};
    };

    using DerivationTraits = ustl::collections::intrusive::DerivationValueTraits<
        DerivedValue,
        ustl::collections::intrusive::ListNodeTraits<void *>>;
    using TrivialTraits = ustl::collections::intrusive::TrivialValueTraits<
        ustl::collections::intrusive::ListNodeTraits<void *>>;
} // namespace

auto main() -> int {
    Value value {.m_prefix = 7, .m_payload = 11};
    const auto node = Traits::to_node_ptr(value);
    const auto recovered = Traits::to_value_ptr(node);
    if (recovered != &value || recovered->m_prefix != 7 || recovered->m_payload != 11) {
        return 1;
    }

    const auto &const_value = value;
    const auto const_node = Traits::to_node_ptr(const_value);
    const auto const_recovered = Traits::to_value_ptr(const_node);
    if (const_recovered != &const_value || const_recovered->m_payload != 11) {
        return 2;
    }

    DirectValue direct_value {.m_prefix = 13, .m_payload = 17};
    const auto direct_node = DirectTraits::to_node_ptr(direct_value);
    const auto direct_recovered = DirectTraits::to_value_ptr(direct_node);
    if (direct_recovered != &direct_value || direct_recovered->m_prefix != 13 || direct_recovered->m_payload != 17) {
        return 3;
    }

    DerivedValue derived_value {.m_payload = 19};
    const auto derived_node = DerivationTraits::to_node_ptr(derived_value);
    const auto derived_recovered = DerivationTraits::to_value_ptr(derived_node);
    if (derived_recovered != &derived_value || derived_recovered->m_payload != 19) {
        return 4;
    }

    ustl::collections::intrusive::ListNode<void *> trivial_value {};
    const auto trivial_node = TrivialTraits::to_node_ptr(trivial_value);
    return TrivialTraits::to_value_ptr(trivial_node) == &trivial_value ? 0 : 5;
}
