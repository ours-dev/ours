/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_TRAITS_INTEGRAL_CONSTANT_HPP
#define USTL_TRAITS_INTEGRAL_CONSTANT_HPP 1

#include <type_traits>

namespace ustl::traits {
template <auto Value> using IntConstant = std::integral_constant<decltype(Value), Value>;
}

#endif // USTL_TRAITS_INTEGRAL_CONSTANT_HPP
