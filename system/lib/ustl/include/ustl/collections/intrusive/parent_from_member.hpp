/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_PARENT_FROM_MEMBER_HPP
#define USTL_COLLECTIONS_INTRUSIVE_PARENT_FROM_MEMBER_HPP 1

#include <ustl/collections/intrusive/detail/parent_from_member.hpp>

namespace ustl::collections::intrusive {
    template <typename Parent, typename Member>
    [[nodiscard]] inline auto get_parent_from_member(Member *const member, Member Parent::*const ptr) noexcept -> Parent * {
        return detail::parent_from_member(member, ptr);
    }

    template <typename Parent, typename Member>
    [[nodiscard]] inline auto get_parent_from_member(Member const *const member, Member Parent::*const ptr) noexcept
        -> Parent const * {
        return detail::parent_from_member(member, ptr);
    }
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_PARENT_FROM_MEMBER_HPP
