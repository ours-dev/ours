/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_TRAITS_CV_HPP
#define USTL_TRAITS_CV_HPP 1

#include <type_traits>

namespace ustl::traits {
template <typename T>
using AddConstT = std::add_const_t<T>;
template <typename T>
using RemoveConstT = std::remove_const_t<T>;
template <typename T>
using AddVolatileT = std::add_volatile_t<T>;
template <typename T>
using RemoveVolatileT = std::remove_volatile_t<T>;
template <typename T>
using AddCvT = std::add_cv_t<T>;
template <typename T>
using RemoveCvT = std::remove_cv_t<T>;
template <typename T>
using RemoveCvRefT = std::remove_cvref_t<T>;
template <typename T>
inline constexpr bool IsConstV = std::is_const_v<T>;
template <typename T>
inline constexpr bool IsVolatileV = std::is_volatile_v<T>;
} // namespace ustl::traits

#endif // USTL_TRAITS_CV_HPP
