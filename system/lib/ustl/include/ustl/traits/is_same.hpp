/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_TRAITS_IS_SAME_HPP
#define USTL_TRAITS_IS_SAME_HPP 1

#include <type_traits>

namespace ustl::traits {

template <typename Left, typename Right>
inline constexpr bool IsSameV = std::is_same_v<Left, Right>;

} // namespace ustl::traits

#endif // USTL_TRAITS_IS_SAME_HPP
