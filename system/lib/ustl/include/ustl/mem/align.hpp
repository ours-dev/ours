/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_MEM_ALIGN_HPP
#define USTL_MEM_ALIGN_HPP 1

#include <ustl/bit.hpp>

#include <type_traits>
#include <utility>

#include <cstddef>

namespace ustl {

namespace detail {
    /// The functions below compute with the mask |alignment| - 1, which is the
    /// bits below the alignment only when the alignment is a power of two. Any
    /// other alignment, 0 among them, gives a wrong answer with no sign of it,
    /// so it stops here: a trap at run time, and a compile error in a constant
    /// expression, which cannot evaluate the trap. An alignment known at compile
    /// time costs nothing; the check folds away.
    constexpr auto require_power_of_two(std::size_t alignment) noexcept -> void {
        if (!has_single_bit(alignment)) {
            __builtin_trap();
        }
    }
} // namespace detail

template <typename T>
constexpr auto round_down(T value, std::size_t alignment) noexcept -> T {
    detail::require_power_of_two(alignment);
    if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(round_down(std::to_underlying(value), alignment));
    } else {
        return static_cast<T>(value & ~static_cast<T>(alignment - 1));
    }
}

/// Checked by round_down().
template <typename T>
constexpr auto round_up(T value, std::size_t alignment) noexcept -> T {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(round_up(std::to_underlying(value), alignment));
    } else {
        return round_down(static_cast<T>(value + static_cast<T>(alignment - 1)), alignment);
    }
}

template <typename T>
constexpr auto is_aligned(T value, std::size_t alignment) noexcept -> bool {
    detail::require_power_of_two(alignment);
    if constexpr (std::is_enum_v<T>) {
        return is_aligned(std::to_underlying(value), alignment);
    } else {
        return (value & static_cast<T>(alignment - 1)) == 0;
    }
}

} // namespace ustl

#endif // USTL_MEM_ALIGN_HPP
