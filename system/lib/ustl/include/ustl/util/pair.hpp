/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_UTIL_PAIR_HPP
#define USTL_UTIL_PAIR_HPP 1

#include <utility>

namespace ustl {

template <typename First, typename Second>
using Pair = std::pair<First, Second>;

using std::forward;
using std::make_pair;
using std::move;
using std::swap;

} // namespace ustl

#endif // USTL_UTIL_PAIR_HPP
