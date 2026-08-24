/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_TRAITS_IS_INVOCABLE_HPP
#define USTL_TRAITS_IS_INVOCABLE_HPP 1

#include <concepts>
#include <type_traits>

namespace ustl::traits {

template <typename Function, typename... Args>
concept Invocable = std::invocable<Function, Args...>;

template <typename Function, typename... Args>
inline constexpr bool IsInvocableV = std::is_invocable_v<Function, Args...>;

template <typename Function, typename... Args>
using InvokeResultT = std::invoke_result_t<Function, Args...>;

} // namespace ustl::traits

#endif // USTL_TRAITS_IS_INVOCABLE_HPP
