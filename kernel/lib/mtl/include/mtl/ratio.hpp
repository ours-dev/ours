/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef MTL_RATIO_HPP
#define MTL_RATIO_HPP 1

#include <ours/assert.hpp>
#include <ours/types.hpp>

#include <mtl/scale.hpp>
#include <ustl/limits.hpp>

namespace mtl {
template <typename Integer_>
class Ratio {
    using Self = Ratio;

  public:
    using Integer = Integer_;

    constexpr Ratio() = default;
    constexpr Ratio(Integer numerator, Integer denominator)
        : m_numerator(numerator), m_denominator(denominator) {}

    template <typename Value>
    constexpr auto scale(Value value) const -> Value {
        DEBUG_ASSERT(m_denominator != 0, "ratio denominator must be nonzero");
        return mtl::scale<RoundOption::Up, ustl::NumericLimits<Value>::max()>(value, m_numerator,
                                                                              m_denominator);
    }

    constexpr auto reduce() -> void {
        mtl::reduce(m_numerator, m_denominator);
    }

    constexpr auto inverse() const -> Self {
        DEBUG_ASSERT(m_numerator != 0, "ratio numerator must be nonzero");
        return Self(m_denominator, m_numerator);
    }

  private:
    Integer m_numerator = 0;
    Integer m_denominator = 1;
};

template <typename Integer>
Ratio(Integer, Integer) -> Ratio<Integer>;
} // namespace mtl

#endif // MTL_RATIO_HPP
