/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_PARENT_FROM_MEMBER_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_PARENT_FROM_MEMBER_HPP 1

#include <memory>
#include <new>

#include <cstddef>
#include <cstdint>

namespace ustl::collections::intrusive::detail {
template <typename Parent, typename Member>
[[nodiscard]]
inline auto member_offset(Member Parent::*const ptr) noexcept -> std::ptrdiff_t {
    // Matches Boost.Intrusive's ABI-level ptr-to-member offset recovery.
    auto const *const parent = static_cast<Parent const *>(nullptr);
    auto const *const member =
        static_cast<char const *>(static_cast<void const *>(&(parent->*ptr)));
    auto const *const base = static_cast<char const *>(static_cast<void const *>(parent));
    return member - base;
}

template <typename Parent, typename Member>
[[nodiscard]]
inline auto parent_from_member(Member *const member, Member Parent::*const ptr) noexcept
    -> Parent * {
    const auto address =
        reinterpret_cast<std::uintptr_t>(member) - static_cast<std::uintptr_t>(member_offset(ptr));
    return std::launder(reinterpret_cast<Parent *>(address));
}

template <typename Parent, typename Member>
[[nodiscard]]
inline auto parent_from_member(Member const *const member, Member Parent::*const ptr) noexcept
    -> Parent const * {
    const auto address =
        reinterpret_cast<std::uintptr_t>(member) - static_cast<std::uintptr_t>(member_offset(ptr));
    return std::launder(reinterpret_cast<Parent const *>(address));
}
} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_PARENT_FROM_MEMBER_HPP
