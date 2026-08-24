/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef MTL_SCALE_HPP
#define MTL_SCALE_HPP 1

#include <utility>

#include <ours/types.hpp>
#include <ustl/limits.hpp>

namespace mtl {
    template <typename Integer>
    constexpr auto binary_gcd(Integer a, Integer b) -> Integer {
        if (a == 0) {
            return b;
        }
        if (b == 0) {
            return a;
        }

        Integer twos = 0;
        while (((a | b) & 1) == 0) {
            a >>= 1;
            b >>= 1;
            ++twos;
        }
        while ((a & 1) == 0) {
            a >>= 1;
        }
        do {
            while ((b & 1) == 0) {
                b >>= 1;
            }
            if (a > b) {
                std::swap(a, b);
            }
            b -= a;
        } while (b != 0);
        return a << twos;
    }

    template <typename Integer>
    constexpr auto reduce(Integer &numerator, Integer &denominator) -> void {
        if (numerator == 0) {
            denominator = 1;
            return;
        }
        const auto divisor = binary_gcd(numerator, denominator);
        if (divisor > 1) {
            numerator /= divisor;
            denominator /= divisor;
        }
    }

    enum class RoundOption { Up, Down };

    template <RoundOption Round, usize OverflowLimit, typename BigInteger, typename Integer>
    constexpr auto scale(BigInteger value, Integer numerator, Integer denominator) -> BigInteger {
        static_assert(Round == RoundOption::Up || Round == RoundOption::Down);
        static_assert(sizeof(BigInteger) > sizeof(Integer));
        static_assert(ustl::NumericLimits<Integer>::DIGITS == 32);

        constexpr auto kBits = usize {32};
        constexpr BigInteger kMask = BigInteger(-1) >> kBits;
        BigInteger high = numerator * (value >> kBits);
        BigInteger low = numerator * (value & kMask);
        high += low >> kBits;
        low &= kMask;

        const auto high_quotient = high / denominator;
        const auto high_remainder = high % denominator;
        constexpr auto OVERFLOW_HIGH = BigInteger(OverflowLimit) >> kBits;
        if (high_quotient > OVERFLOW_HIGH) {
            return BigInteger(OverflowLimit);
        }

        low |= high_remainder << kBits;
        const auto result = (high_quotient << kBits) | (low / denominator);
        if (result >= BigInteger(OverflowLimit)) {
            return BigInteger(OverflowLimit);
        }
        if constexpr (Round == RoundOption::Up) {
            if (low % denominator != 0) {
                return result + 1;
            }
        }
        return result;
    }
} // namespace mtl

#endif // MTL_SCALE_HPP
