/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_TRAITS_IS_BASE_OF_HPP
#define USTL_TRAITS_IS_BASE_OF_HPP 1

#include <type_traits>

namespace ustl::traits {
    template <typename Base, typename Derived>
    using IsBaseOf = std::is_base_of<Base, Derived>;

    template <typename Base, typename Derived>
    inline constexpr bool IsBaseOfV = std::is_base_of_v<Base, Derived>;
} // namespace ustl::traits

#endif // USTL_TRAITS_IS_BASE_OF_HPP
