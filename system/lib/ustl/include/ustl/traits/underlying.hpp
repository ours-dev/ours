/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_TRAITS_UNDERLYING_HPP
#define USTL_TRAITS_UNDERLYING_HPP 1

#include <type_traits>

namespace ustl::traits {
    template <typename T>
    using UnderlyingTypeT = std::underlying_type_t<T>;
} // namespace ustl::traits

#endif // USTL_TRAITS_UNDERLYING_HPP
