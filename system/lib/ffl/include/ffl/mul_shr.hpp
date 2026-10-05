/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FFL_MUL_SHR_HPP
#define FFL_MUL_SHR_HPP 1

#include <ffl/saturating.hpp>
#include <ffl/wider.hpp>

#include <limits>

namespace ffl {

/// `(lhs * rhs) >> shift`, with the product formed in a wider type.
///
/// This is how a scaled multiply is done without a division. A ratio a/b is
/// pre-computed once as `round(2^shift / b)`, and afterwards every scaling by
/// that ratio is one multiply and one shift -- no divide instruction, and on
/// a 64-bit value no call into the compiler runtime at all.
///
/// Worth stating plainly because it is the whole reason this exists: a
/// 128-bit division compiles to `__divti3`, a software loop in
/// libclang_rt.builtins costing tens to hundreds of cycles. On a path that
/// runs on every scheduler tick that is not an implementation detail, it is
/// the cost of the feature.
///
/// The shift happens *before* the narrowing, because the bits being shifted
/// down are exactly the ones that do not fit in the narrow type -- narrowing
/// first would discard the answer and then shift the remains.
template <typename T>
[[nodiscard]]
constexpr auto mul_shr(T lhs, T rhs, usize shift) -> T {
    auto const product = static_cast<Wider<T>>(lhs) * static_cast<Wider<T>>(rhs);
    return clamping_cast<T>(product >> static_cast<Wider<T>>(shift));
}

/// `mul_shr`, rounding the discarded bits to nearest instead of dropping them.
///
/// Prefer this wherever the result is accumulated. A plain shift truncates,
/// and truncation is not a random error -- it is downward every single time,
/// so a value summed across thousands of ticks drifts by roughly half a unit
/// per addition. Scaling 1000 by the ratio 3122/3122 with `mul_shr` answers
/// 999 for exactly this reason.
///
/// Truncating here would accept that bias; this library already chose the
/// other way for Fixed's rounding, and one library should not hold two
/// opinions about the same question.
///
/// A shift of zero discards nothing, so there is nothing to round and the
/// half-way constant would shift out of range.
template <typename T>
[[nodiscard]]
constexpr auto mul_shr_round(T lhs, T rhs, usize shift) -> T {
    auto const product = static_cast<Wider<T>>(lhs) * static_cast<Wider<T>>(rhs);
    if (shift == 0) {
        return clamping_cast<T>(product);
    }

    auto const half = static_cast<Wider<T>>(1) << static_cast<Wider<T>>(shift - 1);
    return clamping_cast<T>((product + half) >> static_cast<Wider<T>>(shift));
}

/// The reciprocal `2^shift / value`, for use as `mul_shr`'s right operand.
///
/// Meant for constant evaluation: the division is the thing being moved off
/// the hot path, so computing one of these at run time would put it back. A
/// caller that needs reciprocals for a fixed set of values builds a table of
/// them at compile time -- see PRIORITY_TO_WMULT_MAP in the scheduler.
///
/// Rounds to nearest rather than truncating. Truncation biases every later
/// product downward, and since the reciprocal is reused for the life of the
/// system that bias is systematic rather than random.
template <typename T>
[[nodiscard]]
constexpr auto reciprocal(T value, usize shift) -> T {
    if (value == T{0}) [[unlikely]] {
        return std::numeric_limits<T>::max();
    }

    auto const numerator = static_cast<Wider<T>>(1) << static_cast<Wider<T>>(shift);
    auto const half = static_cast<Wider<T>>(value) / 2;
    return clamping_cast<T>((numerator + half) / static_cast<Wider<T>>(value));
}

} // namespace ffl

#endif // #ifndef FFL_MUL_SHR_HPP
