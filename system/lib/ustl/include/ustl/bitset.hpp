/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_BITSET_HPP
#define USTL_BITSET_HPP 1

#include <array>

#include <cstddef>
#include <cstdint>

namespace ustl {
template <std::size_t BitCount>
class BitSet {
    static constexpr auto BITS_PER_WORD = std::size_t{64};
    static constexpr auto WORD_COUNT =
        BitCount == 0 ? std::size_t{1} : (BitCount + BITS_PER_WORD - 1) / BITS_PER_WORD;

  public:
    constexpr BitSet() = default;
    constexpr explicit BitSet(std::uint64_t initial) {
        m_words[0] = initial;
        trim();
    }

    [[nodiscard]]
    static constexpr auto size() noexcept -> std::size_t {
        return BitCount;
    }

    [[nodiscard]]
    constexpr auto test(std::size_t index) const noexcept -> bool {
        return index < BitCount && (m_words[word(index)] & bit(index)) != 0;
    }
    [[nodiscard]]
    constexpr auto operator[](std::size_t index) const noexcept -> bool {
        return test(index);
    }

    constexpr auto set(std::size_t index, bool value = true) noexcept -> BitSet & {
        if (index < BitCount) {
            auto &target = m_words[word(index)];
            target = value ? target | bit(index) : target & ~bit(index);
        }
        return *this;
    }
    constexpr auto reset(std::size_t index) noexcept -> BitSet & {
        return set(index, false);
    }
    constexpr auto reset() noexcept -> BitSet & {
        m_words.fill(0);
        return *this;
    }
    constexpr auto clear() noexcept -> void {
        static_cast<void>(reset());
    }

    [[nodiscard]]
    constexpr auto any() const noexcept -> bool {
        for (auto value: m_words) {
            if (value != 0) {
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
    constexpr auto all() const noexcept -> bool {
        if constexpr (BitCount == 0) {
            return true;
        }
        for (std::size_t index = 0; index + 1 < WORD_COUNT; ++index) {
            if (m_words[index] != ~std::uint64_t{0}) {
                return false;
            }
        }
        return m_words[WORD_COUNT - 1] == last_word_mask();
    }
    [[nodiscard]]
    constexpr auto count() const noexcept -> std::size_t {
        std::size_t result = 0;
        for (auto value: m_words) {
            result += static_cast<std::size_t>(__builtin_popcountll(value));
        }
        return result;
    }
    [[nodiscard]]
    constexpr auto test_range(std::size_t start, std::size_t count) const noexcept -> bool {
        return start <= BitCount && count <= BitCount - start && [&] {
            for (std::size_t index = start; index < start + count; ++index) {
                if (!test(index)) {
                    return false;
                }
            }
            return true;
        }();
    }

    template <typename F>
    constexpr auto for_each(F &&function) const -> void {
        for (std::size_t index = 0; index < BitCount; ++index) {
            if (test(index)) {
                function(index);
            }
        }
    }

    constexpr auto operator|=(BitSet const &other) noexcept -> BitSet & {
        for (std::size_t index = 0; index < WORD_COUNT; ++index) {
            m_words[index] |= other.m_words[index];
        }
        return *this;
    }
    constexpr auto operator&=(BitSet const &other) noexcept -> BitSet & {
        for (std::size_t index = 0; index < WORD_COUNT; ++index) {
            m_words[index] &= other.m_words[index];
        }
        return *this;
    }
    constexpr auto operator^=(BitSet const &other) noexcept -> BitSet & {
        for (std::size_t index = 0; index < WORD_COUNT; ++index) {
            m_words[index] ^= other.m_words[index];
        }
        trim();
        return *this;
    }

    friend constexpr auto operator|(BitSet left, BitSet const &right) noexcept -> BitSet {
        return left |= right;
    }
    friend constexpr auto operator&(BitSet left, BitSet const &right) noexcept -> BitSet {
        return left &= right;
    }
    friend constexpr auto operator^(BitSet left, BitSet const &right) noexcept -> BitSet {
        return left ^= right;
    }

  private:
    [[nodiscard]]
    static constexpr auto word(std::size_t index) noexcept -> std::size_t {
        return index / BITS_PER_WORD;
    }
    [[nodiscard]]
    static constexpr auto bit(std::size_t index) noexcept -> std::uint64_t {
        return std::uint64_t{1} << (index % BITS_PER_WORD);
    }
    [[nodiscard]]
    static constexpr auto last_word_mask() noexcept -> std::uint64_t {
        if constexpr (BitCount == 0 || BitCount % BITS_PER_WORD == 0) {
            return ~std::uint64_t{0};
        }
        return (std::uint64_t{1} << (BitCount % BITS_PER_WORD)) - 1;
    }
    constexpr auto trim() noexcept -> void {
        m_words[WORD_COUNT - 1] &= last_word_mask();
    }

    std::array<std::uint64_t, WORD_COUNT> m_words{};
};
} // namespace ustl

#endif // USTL_BITSET_HPP
