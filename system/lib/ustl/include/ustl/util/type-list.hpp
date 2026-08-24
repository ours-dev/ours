/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_UTIL_TYPE_LIST_HPP
#define USTL_UTIL_TYPE_LIST_HPP 1

#include <cstddef>
#include <utility>

namespace ustl {

template <typename... Types>
struct TypeList {
    static constexpr auto size() -> std::size_t { return sizeof...(Types); }
};

template <std::size_t... Indices>
using IndexSequence = std::index_sequence<Indices...>;

template <std::size_t Size>
using MakeIndexSequenceT = std::make_index_sequence<Size>;

}  // namespace ustl

#endif  // USTL_UTIL_TYPE_LIST_HPP
