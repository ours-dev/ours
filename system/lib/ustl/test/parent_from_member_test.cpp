/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/parent_from_member.hpp>

namespace {
    struct Member final {
        int m_value {};
    };

    struct Parent final {
        int m_prefix {};
        Member m_member {};
    };
} // namespace

auto main() -> int {
    Parent parent {.m_prefix = 1, .m_member = {.m_value = 2}};
    const auto *const recovered = ustl::collections::intrusive::get_parent_from_member(&parent.m_member, &Parent::m_member);
    return recovered == &parent && recovered->m_prefix == 1 && recovered->m_member.m_value == 2 ? 0 : 1;
}
