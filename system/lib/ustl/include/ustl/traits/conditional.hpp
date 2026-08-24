/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_TRAITS_CONDITIONAL_HPP
#define USTL_TRAITS_CONDITIONAL_HPP 1

#include <type_traits>

namespace ustl::traits {
    template <bool Condition, typename Then, typename Else>
    using ConditionalT = std::conditional_t<Condition, Then, Else>;
} // namespace ustl::traits

#endif // USTL_TRAITS_CONDITIONAL_HPP
