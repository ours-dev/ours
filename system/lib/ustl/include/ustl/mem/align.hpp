/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_MEM_ALIGN_HPP
#define USTL_MEM_ALIGN_HPP 1

#include <cstddef>

namespace ustl {

template <typename T>
constexpr auto round_down(T value, std::size_t alignment) noexcept -> T {
    return static_cast<T>(value & ~static_cast<T>(alignment - 1));
}

template <typename T>
constexpr auto round_up(T value, std::size_t alignment) noexcept -> T {
    return round_down(static_cast<T>(value + static_cast<T>(alignment - 1)), alignment);
}

template <typename T>
constexpr auto is_aligned(T value, std::size_t alignment) noexcept -> bool {
    return (value & static_cast<T>(alignment - 1)) == 0;
}

} // namespace ustl

#endif // USTL_MEM_ALIGN_HPP
