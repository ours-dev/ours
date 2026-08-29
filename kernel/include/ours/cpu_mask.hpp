/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_CPU_MASK_HPP
#define OURS_CPU_MASK_HPP 1

#include <ours/types.hpp>

#include <array>

namespace ours {
static_assert(MP_MAX_CPUS > 0, "MP_MAX_CPUS must define a non-empty CPU mask");

inline constexpr auto MAX_CPU = usize{MP_MAX_CPUS};

class CpuMask final {
  public:
    static constexpr auto BIT_COUNT = MAX_CPU;
    static constexpr auto WORD_BITS = sizeof(usize) * 8;
    static constexpr auto WORD_COUNT = (BIT_COUNT + WORD_BITS - 1) / WORD_BITS;

    constexpr CpuMask() = default;

    [[nodiscard]]
    static constexpr auto of(CpuNum cpu) -> CpuMask {
        CpuMask result;
        result.set(cpu);
        return result;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return BIT_COUNT;
    }

    [[nodiscard]]
    constexpr auto test(CpuNum cpu) const noexcept -> bool {
        return valid(cpu) && (words_[word(cpu)] & bit(cpu)) != 0;
    }

    constexpr auto set(CpuNum cpu, bool value = true) noexcept -> CpuMask & {
        if (!valid(cpu)) {
            return *this;
        }
        if (value) {
            words_[word(cpu)] |= bit(cpu);
        } else {
            words_[word(cpu)] &= ~bit(cpu);
        }
        return *this;
    }

    constexpr auto reset() noexcept -> CpuMask & {
        for (auto &word: words_) {
            word = 0;
        }
        return *this;
    }

    [[nodiscard]]
    constexpr auto any() const noexcept -> bool {
        for (auto word: words_) {
            if (word != 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    constexpr auto none() const noexcept -> bool {
        return !any();
    }

    [[nodiscard]]
    constexpr auto count() const noexcept -> usize {
        auto result = usize{0};
        for (auto word: words_) {
            while (word != 0) {
                word &= word - 1;
                ++result;
            }
        }
        return result;
    }

    template <typename Function>
    constexpr auto for_each(Function &&function) const -> void {
        for (CpuNum cpu = 0; cpu < BIT_COUNT; ++cpu) {
            if (test(cpu)) {
                function(cpu);
            }
        }
    }

    constexpr auto operator&=(CpuMask const &other) noexcept -> CpuMask & {
        for (usize index = 0; index < WORD_COUNT; ++index) {
            words_[index] &= other.words_[index];
        }
        return *this;
    }

    constexpr auto operator|=(CpuMask const &other) noexcept -> CpuMask & {
        for (usize index = 0; index < WORD_COUNT; ++index) {
            words_[index] |= other.words_[index];
        }
        return *this;
    }

    constexpr auto operator^=(CpuMask const &other) noexcept -> CpuMask & {
        for (usize index = 0; index < WORD_COUNT; ++index) {
            words_[index] ^= other.words_[index];
        }
        return *this;
    }

    [[nodiscard]]
    friend constexpr auto operator&(CpuMask left, CpuMask const &right) noexcept -> CpuMask {
        return left &= right;
    }

    [[nodiscard]]
    friend constexpr auto operator|(CpuMask left, CpuMask const &right) noexcept -> CpuMask {
        return left |= right;
    }

    [[nodiscard]]
    friend constexpr auto operator^(CpuMask left, CpuMask const &right) noexcept -> CpuMask {
        return left ^= right;
    }

    [[nodiscard]]
    friend constexpr auto operator==(CpuMask const &, CpuMask const &) noexcept -> bool = default;

  private:
    [[nodiscard]]
    static constexpr auto valid(CpuNum cpu) noexcept -> bool {
        return cpu < BIT_COUNT;
    }

    [[nodiscard]]
    static constexpr auto word(CpuNum cpu) noexcept -> usize {
        return static_cast<usize>(cpu) / WORD_BITS;
    }

    [[nodiscard]]
    static constexpr auto bit(CpuNum cpu) noexcept -> usize {
        return usize{1} << (static_cast<usize>(cpu) % WORD_BITS);
    }

    std::array<usize, WORD_COUNT> words_{};
};
} // namespace ours

#endif // OURS_CPU_MASK_HPP
