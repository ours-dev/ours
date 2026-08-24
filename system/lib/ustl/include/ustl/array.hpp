/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_ARRAY_HPP
#define USTL_ARRAY_HPP 1

#include <array>
#include <cstddef>

namespace ustl {

template <typename T, std::size_t N>
using Array = std::array<T, N>;

}  // namespace ustl

#endif  // USTL_ARRAY_HPP
