/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_DETAIL_ASSERT_HPP
#define USTL_COLLECTIONS_INTRUSIVE_DETAIL_ASSERT_HPP 1

namespace ustl::collections::intrusive::detail {
    constexpr auto require(bool condition) noexcept -> void {
        if (!condition) {
            __builtin_trap();
        }
    }
} // namespace ustl::collections::intrusive::detail

#endif // USTL_COLLECTIONS_INTRUSIVE_DETAIL_ASSERT_HPP
