/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FFL_FIXED_HPP
#define FFL_FIXED_HPP 1

#include <ffl/format.hpp>
#include <ffl/mul_shr.hpp>
#include <ffl/saturating.hpp>
#include <ffl/wider.hpp>

namespace ffl {

/// A number with `FractionalBits` bits after the binary point, stored in
/// `Integer`.
///
/// Exists because the kernel has no floating point (see the note on
/// -msoft-float in //system/BUILD.gn) and integer division throws away
/// precisely the part the scheduler needs. `weight / MID_WEIGHT` as plain
/// integers is 0 for every below-average weight, which is how a thread's
/// weighted time became zero regardless of how long it ran.
///
/// Deliberately, there are no expression templates here. They defer
/// evaluation so that a compound expression can choose its intermediate
/// precision from the type it is finally assigned to. That is worth the
/// machinery when expressions are long; the arithmetic in this kernel's
/// scheduler is binary, and an explicit widening intermediate (see Wider)
/// covers it without asking every reader to understand a lazily evaluated
/// tree. When a three-term chain appears, revisit this rather than nesting
/// casts.
template <typename Integer, usize FractionalBits>
class Fixed {
    using Self = Fixed;

  public:
    using Format = ffl::Format<Integer, FractionalBits>;
    using Element = Integer;

    static constexpr usize FRACTIONAL_BITS = FractionalBits;

    constexpr Fixed() = default;

    /// From a whole number. Explicit: `Fixed<i64, 8> x = 3;` reads as though 3
    /// were already the stored representation, and it is not -- it is 3 << 8.
    explicit constexpr Fixed(Integer whole) : m_raw(saturating_mul<Integer>(whole, Format::ONE)) {}

    /// From another format, rescaling rather than reinterpreting.
    ///
    /// Explicit, and deliberately so: Fixed<i64, 8> and Fixed<i64, 16> hold
    /// different numbers in the same bits, and a conversion that happened
    /// silently would let one be passed where the other is expected with no
    /// diagnostic and no visible cast. Widening the fraction is exact;
    /// narrowing it rounds, half to even, by the same argument as round().
    template <typename OtherInteger, usize OtherFractionalBits>
    explicit constexpr Fixed(Fixed<OtherInteger, OtherFractionalBits> other) {
        using Intermediate = Wider<Integer>;
        auto const raw = static_cast<Intermediate>(other.raw());

        if constexpr (OtherFractionalBits <= FractionalBits) {
            constexpr auto GAIN = FractionalBits - OtherFractionalBits;
            m_raw = clamping_cast<Integer>(raw << static_cast<Intermediate>(GAIN));
        } else {
            // Rounding a right shift by hand: the shift alone would truncate,
            // and truncation of a conversion is a bias every later use
            // inherits.
            constexpr auto LOSS = OtherFractionalBits - FractionalBits;
            constexpr auto HALF = static_cast<Intermediate>(1)
                                  << static_cast<Intermediate>(LOSS - 1);
            m_raw = clamping_cast<Integer>((raw + HALF) >> static_cast<Intermediate>(LOSS));
        }
    }

    /// Wraps a value that is *already* in the stored representation.
    [[nodiscard]]
    static constexpr auto from_raw(Integer raw) -> Self {
        Self result;
        result.m_raw = raw;
        return result;
    }

    /// The exact ratio `numerator / denominator`, to the precision of this
    /// format.
    ///
    /// This is what the scheduler wants and what plain integers cannot give
    /// it: the numerator is shifted up into the fractional range *before* the
    /// division, so a ratio below one keeps its value instead of flooring to
    /// zero. The shift happens in the wider type, because `numerator <<
    /// FractionalBits` is exactly the overflow the widening exists to absorb.
    ///
    /// A zero denominator yields the saturated value rather than trapping:
    /// this runs on the scheduler's hot path where a check the caller has
    /// already made would be paid for twice, and "infinitely large ratio" is
    /// the answer that keeps the surrounding comparisons ordered.
    [[nodiscard]]
    static constexpr auto from_ratio(Integer numerator, Integer denominator) -> Self {
        if (denominator == Integer{0}) [[unlikely]] {
            return from_raw(numerator < Integer{0} ? Format::MIN : Format::MAX);
        }

        auto const scaled = static_cast<Wider<Integer>>(numerator)
                            << static_cast<Wider<Integer>>(FractionalBits);
        return from_raw(clamping_cast<Integer>(scaled / static_cast<Wider<Integer>>(denominator)));
    }

    [[nodiscard]]
    static constexpr auto min() -> Self {
        return from_raw(Format::MIN);
    }

    [[nodiscard]]
    static constexpr auto max() -> Self {
        return from_raw(Format::MAX);
    }

    /// The smallest representable step -- one unit in the last place.
    [[nodiscard]]
    static constexpr auto epsilon() -> Self {
        return from_raw(Integer{1});
    }

    [[nodiscard]]
    constexpr auto raw() const -> Integer {
        return m_raw;
    }

    /// Nearest whole number, ties to even. See Format::round_to_integer.
    [[nodiscard]]
    constexpr auto round() const -> Integer {
        return Format::round_to_integer(m_raw);
    }

    /// Largest whole number not greater than this value.
    ///
    /// A right shift, not a division: for a signed Integer `>>` is arithmetic
    /// and so floors, whereas `/` truncates toward zero and would round -0.5
    /// up to 0.
    [[nodiscard]]
    constexpr auto floor() const -> Integer {
        return static_cast<Integer>(m_raw >> FractionalBits);
    }

    [[nodiscard]]
    constexpr auto ceiling() const -> Integer {
        if ((m_raw & Format::FRACTIONAL_MASK) == Integer{0}) {
            return floor();
        }
        return saturating_add<Integer>(floor(), 1);
    }

    constexpr auto operator+(Self rhs) const -> Self {
        return from_raw(saturating_add<Integer>(m_raw, rhs.m_raw));
    }

    constexpr auto operator-(Self rhs) const -> Self {
        return from_raw(saturating_sub<Integer>(m_raw, rhs.m_raw));
    }

    // Addition and subtraction are deliberately NOT generalised across
    // formats the way multiplication and division are above. Multiplying by
    // another format is *scaling*, and is meaningful between unlike
    // quantities -- a duration by a weight. Adding them means the two are the
    // same kind of quantity carried at different precision, which is a
    // conversion, and one worth making the caller write out: `duration +
    // weight` is a bug in every program that contains it, and an implicit
    // rescale would compile it.

    constexpr auto operator-() const -> Self {
        return from_raw(saturating_sub<Integer>(Integer{0}, m_raw));
    }

    /// Product, rescaled back to *this* format.
    ///
    /// Two values scaled by 2^f1 and 2^f2 multiply to something scaled by
    /// 2^(f1+f2), so the result is shifted back down by the right operand's
    /// f2 to land in the left operand's format.
    ///
    /// **The left operand's format wins.** That rule is not a coin toss: it is
    /// forced by `operator*=`, which can only ever produce the format of the
    /// object it assigns to, and `a * b` had better agree with `a *= b`. It
    /// also reads the way the call sites mean it -- `duration * weight` is
    /// "scale this duration by that fraction", and the answer is a duration.
    ///
    /// Expression templates would answer this better, by not answering it
    /// eagerly at all: they defer the multiply and let the assignment target
    /// pick the intermediate precision. That machinery is what the top of this
    /// file declines. Where the eager rule costs precision -- a long chain,
    /// or an intermediate that needs more fractional bits than either
    /// operand -- convert explicitly and say why.
    template <usize OtherFractionalBits>
    constexpr auto operator*(Fixed<Integer, OtherFractionalBits> rhs) const -> Self {
        // Rounds to nearest. Truncating here errs downward every time, and
        // these products are accumulated (a virtual runtime is nothing but a
        // sum of them), so the error would grow with the number of terms
        // instead of cancelling.
        return from_raw(mul_shr_round<Integer>(m_raw, rhs.raw(), OtherFractionalBits));
    }

    /// Scales by a whole number, with no rescaling: the scale factor of the
    /// operand is 1, so the product is already in this format.
    constexpr auto operator*(Integer scalar) const -> Self {
        auto const product =
            static_cast<Wider<Integer>>(m_raw) * static_cast<Wider<Integer>>(scalar);
        return from_raw(clamping_cast<Integer>(product));
    }

    /// Quotient, rescaled back to *this* format. The left operand's format
    /// wins here too, for the same reason `operator/=` forces it.
    ///
    /// The two scale factors cancel, so the numerator has to be shifted up by
    /// the divisor's f2 first or the fraction is lost -- the mirror of
    /// from_ratio(), and the same reason the shift happens in the wider type.
    template <usize OtherFractionalBits>
    constexpr auto operator/(Fixed<Integer, OtherFractionalBits> rhs) const -> Self {
        if (rhs.raw() == Integer{0}) [[unlikely]] {
            return from_raw(m_raw < Integer{0} ? Format::MIN : Format::MAX);
        }

        auto const scaled = static_cast<Wider<Integer>>(m_raw)
                            << static_cast<Wider<Integer>>(OtherFractionalBits);
        return from_raw(clamping_cast<Integer>(scaled / static_cast<Wider<Integer>>(rhs.raw())));
    }

    constexpr auto operator+=(Self rhs) -> Self & {
        *this = *this + rhs;
        return *this;
    }

    constexpr auto operator-=(Self rhs) -> Self & {
        *this = *this - rhs;
        return *this;
    }

    /// Divides by a whole number. No rescaling: the divisor's scale factor is
    /// 1, so the quotient is already in this format -- the mirror of
    /// operator*(Integer).
    constexpr auto operator/(Integer scalar) const -> Self {
        if (scalar == Integer{0}) [[unlikely]] {
            return from_raw(m_raw < Integer{0} ? Format::MIN : Format::MAX);
        }
        return from_raw(static_cast<Integer>(m_raw / scalar));
    }

    template <usize OtherFractionalBits>
    constexpr auto operator*=(Fixed<Integer, OtherFractionalBits> rhs) -> Self & {
        *this = *this * rhs;
        return *this;
    }

    constexpr auto operator*=(Integer scalar) -> Self & {
        *this = *this * scalar;
        return *this;
    }

    template <usize OtherFractionalBits>
    constexpr auto operator/=(Fixed<Integer, OtherFractionalBits> rhs) -> Self & {
        *this = *this / rhs;
        return *this;
    }

    constexpr auto operator/=(Integer scalar) -> Self & {
        *this = *this / scalar;
        return *this;
    }

    /// Magnitude. Saturates rather than wrapping: the most negative value of a
    /// two's-complement type has no positive counterpart, and returning it
    /// unchanged -- which is what negation does there -- would be a negative
    /// "absolute value".
    [[nodiscard]]
    constexpr auto absolute() const -> Self {
        if (m_raw >= Integer{0}) {
            return *this;
        }
        if (m_raw == Format::MIN) {
            return from_raw(Format::MAX);
        }
        return from_raw(static_cast<Integer>(-m_raw));
    }

    /// Ordering is the raw values' ordering, because the scale factor is a
    /// positive constant common to both -- which is the property that makes a
    /// fixed-point key usable directly in an ordered container.
    constexpr auto operator<=>(Self const &) const = default;
    constexpr auto operator==(Self const &) const -> bool = default;

  private:
    Integer m_raw{};
};

/// `scalar * fixed`, so that a whole number may be written on either side.
/// The member operator covers `fixed * scalar`; this is the mirror, and it
/// multiplies rather than reimplementing anything.
template <typename Integer, usize FractionalBits>
[[nodiscard]]
constexpr auto operator*(Integer lhs, Fixed<Integer, FractionalBits> rhs)
    -> Fixed<Integer, FractionalBits> {
    return rhs * lhs;
}

} // namespace ffl

#endif // #ifndef FFL_FIXED_HPP
