/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_PRIORITY_COMPARE_HPP
#define USTL_COLLECTIONS_INTRUSIVE_PRIORITY_COMPARE_HPP 1

namespace ustl::collections::intrusive {
namespace adldft {
    template <typename T, typename U>
    [[nodiscard]]
    constexpr auto priority_order(T const &left, U const &right) noexcept(noexcept(left < right))
        -> bool {
        return left < right;
    }
} // namespace adldft

template <typename T>
struct PriorityCompare final {
    [[nodiscard]]
    constexpr auto operator()(T const &left, T const &right) const noexcept -> bool {
        using adldft::priority_order;
        return priority_order(left, right);
    }
};

template <>
struct PriorityCompare<void> final {
    template <typename T, typename U>
    [[nodiscard]]
    constexpr auto operator()(T const &left, U const &right) const noexcept -> bool {
        using adldft::priority_order;
        return priority_order(left, right);
    }
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_PRIORITY_COMPARE_HPP
