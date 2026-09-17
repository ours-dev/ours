/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FFL_FIXED_HPP
#define FFL_FIXED_HPP 1

#include <ffl/format.hpp>
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
/// Deliberately not Zircon's ffl in one respect: there are no expression
/// templates here. ffl defers evaluation so that a compound expression can
/// choose its intermediate precision from the type it is finally assigned to.
/// That is worth its 492 lines when expressions are long; the arithmetic in
/// this kernel's scheduler is binary, and an explicit widening intermediate
/// (see Wider) covers it without asking every reader to understand a lazily
/// evaluated tree. When a three-term chain appears, revisit this rather than
/// nesting casts.
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

    constexpr auto operator-() const -> Self {
        return from_raw(saturating_sub<Integer>(Integer{0}, m_raw));
    }

    /// Product, rescaled back to this format.
    ///
    /// Two values each scaled by 2^f multiply to something scaled by 2^2f, so
    /// the result is shifted back down by f. Both steps happen in the wider
    /// type: the unshifted product is what overflows, and shifting after the
    /// narrowing would discard the very bits being shifted into place.
    constexpr auto operator*(Self rhs) const -> Self {
        auto const product =
            static_cast<Wider<Integer>>(m_raw) * static_cast<Wider<Integer>>(rhs.m_raw);
        return from_raw(
            clamping_cast<Integer>(product >> static_cast<Wider<Integer>>(FractionalBits)));
    }

    /// Scales by a whole number, with no rescaling: the scale factor of the
    /// operand is 1, so the product is already in this format.
    constexpr auto operator*(Integer scalar) const -> Self {
        auto const product =
            static_cast<Wider<Integer>>(m_raw) * static_cast<Wider<Integer>>(scalar);
        return from_raw(clamping_cast<Integer>(product));
    }

    /// Quotient, rescaled back to this format.
    ///
    /// The two scale factors cancel, so the numerator has to be shifted up by
    /// f first or the fraction is lost -- the mirror of from_ratio(), and the
    /// same reason the shift happens in the wider type.
    constexpr auto operator/(Self rhs) const -> Self {
        if (rhs.m_raw == Integer{0}) [[unlikely]] {
            return from_raw(m_raw < Integer{0} ? Format::MIN : Format::MAX);
        }

        auto const scaled = static_cast<Wider<Integer>>(m_raw)
                            << static_cast<Wider<Integer>>(FractionalBits);
        return from_raw(clamping_cast<Integer>(scaled / static_cast<Wider<Integer>>(rhs.m_raw)));
    }

    constexpr auto operator+=(Self rhs) -> Self & {
        *this = *this + rhs;
        return *this;
    }

    constexpr auto operator-=(Self rhs) -> Self & {
        *this = *this - rhs;
        return *this;
    }

    /// Ordering is the raw values' ordering, because the scale factor is a
    /// positive constant common to both -- which is the property that makes a
    /// fixed-point key usable directly in an ordered container.
    constexpr auto operator<=>(Self const &) const = default;
    constexpr auto operator==(Self const &) const -> bool = default;

  private:
    Integer m_raw{};
};

} // namespace ffl

#endif // #ifndef FFL_FIXED_HPP
