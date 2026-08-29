/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_LIMITS_HPP
#define USTL_LIMITS_HPP 1

#include <limits>

namespace ustl {

template <typename T>
struct NumericLimits: std::numeric_limits<T> {
    using Base = std::numeric_limits<T>;
    static constexpr auto DIGITS = Base::digits;
    static constexpr auto DIGITS10 = Base::digits10;
};

} // namespace ustl

#endif // USTL_LIMITS_HPP
