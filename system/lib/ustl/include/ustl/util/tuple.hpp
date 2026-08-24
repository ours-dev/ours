/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_UTIL_TUPLE_HPP
#define USTL_UTIL_TUPLE_HPP 1

#include <tuple>

namespace ustl {

template <typename... Types>
using Tuple = std::tuple<Types...>;

using std::get;
using std::make_tuple;

} // namespace ustl

#endif // USTL_UTIL_TUPLE_HPP
