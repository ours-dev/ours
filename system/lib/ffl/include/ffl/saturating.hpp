/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FFL_SATURATING_HPP
#define FFL_SATURATING_HPP 1

#include <limits>
#include <type_traits>
#include <utility>

namespace ffl {

/// Arithmetic that clamps at the type's limits instead of wrapping.
///
/// Signed overflow is undefined behaviour, so it cannot be detected after the
/// fact by inspecting the result -- the compiler is entitled to assume it did
/// not happen and optimise on that basis. These use the `__builtin_*_overflow`
/// intrinsics, which compute into a wider internal type and report whether the
/// value fitted, which is the only portable way to ask.
///
/// Saturation rather than wrapping is what a fixed-point value wants: a
/// scheduler weight that overflows should become "the largest weight", not a
/// small one. A wrapped weight inverts the very ordering the arithmetic exists
/// to compute.
///
/// The counterpart is Zircon's ffl/saturating_arithmetic.h.
template <typename T>
[[nodiscard]]
constexpr auto saturating_add(T lhs, T rhs) -> T {
    T result{};
    if (!__builtin_add_overflow(lhs, rhs, &result)) [[likely]] {
        return result;
    }

    // Overflow direction is decided by the operands, not by the wrapped
    // result: for addition the sum can only have run off the end the addends
    // were pointing at.
    if (lhs < T{} || rhs < T{}) {
        return std::numeric_limits<T>::min();
    }
    return std::numeric_limits<T>::max();
}

template <typename T>
[[nodiscard]]
constexpr auto saturating_sub(T lhs, T rhs) -> T {
    T result{};
    if (!__builtin_sub_overflow(lhs, rhs, &result)) [[likely]] {
        return result;
    }

    // a - b overflows upward only when a is non-negative and b is negative.
    if (lhs >= T{} && rhs < T{}) {
        return std::numeric_limits<T>::max();
    }
    return std::numeric_limits<T>::min();
}

template <typename T>
[[nodiscard]]
constexpr auto saturating_mul(T lhs, T rhs) -> T {
    T result{};
    if (!__builtin_mul_overflow(lhs, rhs, &result)) [[likely]] {
        return result;
    }

    // The sign of a product is the product of the signs, and that is knowable
    // without the magnitude. `<` against zero rather than a sign test so this
    // compiles for unsigned T too, where the negative branches fold away.
    auto const negative = (lhs < T{}) != (rhs < T{});
    if (negative) {
        return std::numeric_limits<T>::min();
    }
    return std::numeric_limits<T>::max();
}

/// Narrows `value` to `To`, clamping rather than truncating.
///
/// A plain static_cast on an out-of-range value is implementation-defined for
/// signed types and silently modular for unsigned ones; neither is what a
/// saturating computation wants at its last step.
template <typename To, typename From>
[[nodiscard]]
constexpr auto clamping_cast(From value) -> To {
    constexpr auto TO_MIN = std::numeric_limits<To>::min();
    constexpr auto TO_MAX = std::numeric_limits<To>::max();

    // Compared in a type wide enough to hold both, so that the comparison
    // itself does not do the narrowing it is trying to prevent. std::cmp_*
    // handles the signed/unsigned pairing that would otherwise convert one
    // side behind our back.
    if (std::cmp_less(value, TO_MIN)) {
        return TO_MIN;
    }
    if (std::cmp_greater(value, TO_MAX)) {
        return TO_MAX;
    }
    return static_cast<To>(value);
}

} // namespace ffl

#endif // #ifndef FFL_SATURATING_HPP
