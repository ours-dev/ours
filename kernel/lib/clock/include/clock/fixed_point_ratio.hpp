/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef CLOCK_FIXED_POINT_RATIO_HPP
#define CLOCK_FIXED_POINT_RATIO_HPP 1

#include <ours/result.hpp>
#include <ours/types.hpp>

namespace clock {
/// Represents a binary fixed-point ratio in the form multiplier / 2^shift.
///
/// Callers construct it from any rational scale and then use scale() to
/// convert values. The encoded multiplier and shift stay private so the
/// rounding and validation rules remain local.
class FixedPointRatio final {
    using Self = FixedPointRatio;
    using Wide = unsigned __int128;

  public:
    struct Constraints {
        constexpr Constraints(u64 const max_multiplier = u64{0xffffffff}, u64 const max_input = 0,
                              u64 const max_output = ~u64{0}, u8 const max_shift = 32)
            : max_multiplier(max_multiplier)
            , max_input(max_input)
            , max_output(max_output)
            , max_shift(max_shift) {}

        u64 max_multiplier;
        u64 max_input;
        u64 max_output;
        u8 max_shift;
    };

    constexpr FixedPointRatio() = default;

    [[nodiscard]]
    static constexpr auto from_ratio(u64 const numerator, u64 const denominator,
                                     Constraints const constraints = {}) -> ours::Result<Self>;

    [[nodiscard]]
    constexpr auto scale(u64 const value) const -> u64 {
        return static_cast<u64>((Wide(value) * m_multiplier) >> m_shift);
    }

    [[nodiscard]]
    constexpr auto scale_ceil(u64 const value) const -> u64 {
        auto const product = Wide(value) * m_multiplier;
        auto const rounder = (Wide(1) << m_shift) - 1;
        return static_cast<u64>((product + rounder) >> m_shift);
    }

    [[nodiscard]]
    constexpr auto valid() const -> bool {
        return m_multiplier != 0;
    }

  private:
    static constexpr auto MAX_SHIFT = u8{63};

    constexpr FixedPointRatio(u64 const multiplier, u8 const shift)
        : m_multiplier(multiplier), m_shift(shift) {}

    [[nodiscard]]
    static constexpr auto rounded_div(Wide const numerator, u64 const denominator) -> Wide {
        return (numerator + denominator / 2) / denominator;
    }

    [[nodiscard]]
    constexpr auto scale_wide(u64 const value) const -> Wide {
        return (Wide(value) * m_multiplier) >> m_shift;
    }

    u64 m_multiplier = 0;
    u8 m_shift = 0;
};

[[nodiscard]]
constexpr auto FixedPointRatio::from_ratio(u64 const numerator, u64 const denominator,
                                           Constraints const constraints) -> ours::Result<Self> {
    using ours::Status;

    if (numerator == 0 || denominator == 0 || constraints.max_shift > MAX_SHIFT) {
        ustl::err(Status::InvalidArgument);
    }

    for (auto shift = constraints.max_shift + 1; shift > 0; --shift) {
        auto const candidate_shift = static_cast<u8>(shift - 1);
        auto const multiplier = rounded_div(Wide(numerator) << candidate_shift, denominator);
        if (multiplier == 0 || multiplier > constraints.max_multiplier) {
            continue;
        }

        auto const ratio = Self(static_cast<u64>(multiplier), candidate_shift);
        if (constraints.max_input != 0 &&
            ratio.scale_wide(constraints.max_input) > constraints.max_output) {
            continue;
        }

        return ustl::ok(ratio);
    }

    return ustl::err(Status::OutOfRange);
}

} // namespace clock

#endif // CLOCK_FIXED_POINT_RATIO_HPP
