/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_TRAITS_INTEGRAL_HPP
#define USTL_TRAITS_INTEGRAL_HPP 1

#include <type_traits>

namespace ustl::traits {

template <typename T>
using IsIntegral = std::is_integral<T>;

template <typename T>
inline constexpr bool IsIntegralV = std::is_integral_v<T>;

} // namespace ustl::traits

#endif // USTL_TRAITS_INTEGRAL_HPP
