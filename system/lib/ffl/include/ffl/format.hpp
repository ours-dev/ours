/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FFL_FORMAT_HPP
#define FFL_FORMAT_HPP 1

#include <ffl/saturating.hpp>

#include <limits>
#include <type_traits>

namespace ffl {

/// Describes one fixed-point layout: how an `Integer` is split between whole
/// and fractional bits, and the primitive operations that depend on that split.
///
/// Kept apart from `Fixed` because the arithmetic needs to talk about a layout
/// without holding a value of it -- a multiplication of two differently-scaled
/// values has to reason about three formats at once.
template <typename Integer, usize FractionalBits>
struct Format {
    static_assert(std::is_integral_v<Integer>, "a fixed-point value is an integer underneath");
    static_assert(FractionalBits < sizeof(Integer) * 8,
                  "the fractional part must leave at least one bit for the whole part, "
                  "including the sign bit of a signed type");

    using Element = Integer;

    static constexpr usize TOTAL_BITS = sizeof(Integer) * 8;
    static constexpr usize FRACTIONAL_BITS = FractionalBits;
    static constexpr usize INTEGRAL_BITS = TOTAL_BITS - FRACTIONAL_BITS;

    static constexpr bool IS_SIGNED = std::is_signed_v<Integer>;

    /// 2^FractionalBits -- the raw value that represents 1.
    ///
    /// Computed on the *unsigned* counterpart and cast back. With
    /// FractionalBits one below the width of a signed Integer, `1 <<
    /// FractionalBits` shifts into the sign bit, which is undefined behaviour;
    /// the unsigned shift is defined and the cast that follows is
    /// implementation-defined but universally two's-complement here.
    static constexpr Integer ONE =
        static_cast<Integer>(std::make_unsigned_t<Integer>{1} << FractionalBits);

    static constexpr Integer FRACTIONAL_MASK =
        static_cast<Integer>((std::make_unsigned_t<Integer>{1} << FractionalBits) - 1);

    static constexpr Integer MIN = std::numeric_limits<Integer>::min();
    static constexpr Integer MAX = std::numeric_limits<Integer>::max();

    /// Rounds a raw value to a whole number, half to even.
    ///
    /// Half-to-even ("convergent", "banker's") rather than half-away-from-zero
    /// because these values are accumulated: a scheduler adds weighted
    /// durations into a running virtual time thousands of times a second, and
    /// any rule that breaks ties in a fixed direction accumulates a drift
    /// proportional to the number of additions. Half-to-even breaks ties both
    /// ways in equal measure, so the error stays bounded instead of growing.
    /// This is ffl's stated reason for the same choice, and IEEE 754's default
    /// for the same one.
    [[nodiscard]]
    static constexpr auto round_to_integer(Integer raw) -> Integer {
        if constexpr (FractionalBits == 0) {
            return raw;
        } else {
            auto const fraction = static_cast<Integer>(raw & FRACTIONAL_MASK);
            auto const whole = static_cast<Integer>(raw >> FractionalBits);

            // Note the shift above is arithmetic for signed Integer, so it
            // floors rather than truncating toward zero. Everything below is
            // phrased in terms of that floor, which is what makes one rule
            // cover negatives without a separate sign branch.
            constexpr Integer HALF = static_cast<Integer>(ONE >> 1);
            if (fraction < HALF) {
                return whole;
            }
            if (fraction > HALF) {
                return saturating_add<Integer>(whole, 1);
            }

            // Exactly half: round to the even neighbour.
            if ((whole & Integer{1}) == Integer{0}) {
                return whole;
            }
            return saturating_add<Integer>(whole, 1);
        }
    }
};

} // namespace ffl

#endif // #ifndef FFL_FORMAT_HPP
