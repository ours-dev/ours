/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_BITSET_HPP
#define USTL_BITSET_HPP 1

#include <ustl/bit.hpp>

#include <array>
#include <limits>
#include <type_traits>

#include <cstddef>
#include <cstdint>

namespace ustl {
template <std::size_t BitCount, typename Index = std::size_t>
class BitSet final {
    static_assert(std::is_integral_v<Index>, "BitSet index type must be an integer");
    static_assert(std::is_unsigned_v<Index>, "BitSet index type must be unsigned");
    static_assert(BitCount == 0 || static_cast<std::uintmax_t>(std::numeric_limits<Index>::max()) >=
                                       BitCount - 1,
                  "BitSet index type cannot address every bit");

    using Word = std::uint64_t;

    static constexpr auto BITS_PER_WORD = std::size_t{64};

    [[nodiscard]]
    static consteval auto compute_word_count() noexcept -> std::size_t {
        if (BitCount == 0) {
            return std::size_t{1};
        }
        return (BitCount + BITS_PER_WORD - 1) / BITS_PER_WORD;
    }

    static constexpr auto WORD_COUNT = compute_word_count();

  public:
    using Size = std::size_t;

    static constexpr auto BIT_COUNT = BitCount;

    constexpr BitSet() noexcept = default;

    constexpr explicit BitSet(Word initial) noexcept {
        m_words[0] = initial;
        trim();
    }

    [[nodiscard]]
    static constexpr auto of(Size index) noexcept -> BitSet {
        BitSet result;
        result.set(index);
        return result;
    }

    [[nodiscard]]
    static constexpr auto size() noexcept -> Size {
        return BitCount;
    }

    [[nodiscard]]
    constexpr auto test(Size index) const noexcept -> bool {
        if (!in_range(index)) {
            return false;
        }
        return (m_words[word_index(index)] & bit_mask(index)) != 0;
    }

    [[nodiscard]]
    constexpr auto operator[](Size index) const noexcept -> bool {
        return test(index);
    }

    constexpr auto set(Size index, bool value = true) noexcept -> BitSet & {
        if (!in_range(index)) {
            return *this;
        }
        auto &target = m_words[word_index(index)];
        auto const mask = bit_mask(index);
        if (value) {
            target |= mask;
        } else {
            target &= ~mask;
        }
        return *this;
    }

    constexpr auto set() noexcept -> BitSet & {
        m_words.fill(~Word{0});
        trim();
        return *this;
    }

    constexpr auto reset(Size index) noexcept -> BitSet & {
        return set(index, false);
    }

    constexpr auto reset() noexcept -> BitSet & {
        m_words.fill(Word{0});
        return *this;
    }

    constexpr auto clear() noexcept -> void {
        static_cast<void>(reset());
    }

    constexpr auto flip(Size index) noexcept -> BitSet & {
        if (!in_range(index)) {
            return *this;
        }
        m_words[word_index(index)] ^= bit_mask(index);
        return *this;
    }

    constexpr auto flip() noexcept -> BitSet & {
        for (auto &word: m_words) {
            word = ~word;
        }
        trim();
        return *this;
    }

    [[nodiscard]]
    constexpr auto any() const noexcept -> bool {
        for (auto const word: m_words) {
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
    constexpr auto all() const noexcept -> bool {
        if constexpr (BitCount == 0) {
            return true;
        }
        for (Size index = 0; index + 1 < WORD_COUNT; ++index) {
            if (m_words[index] != ~Word{0}) {
                return false;
            }
        }
        return m_words[WORD_COUNT - 1] == last_word_mask();
    }

    [[nodiscard]]
    constexpr auto count() const noexcept -> Size {
        auto result = Size{0};
        for (auto const word: m_words) {
            result += static_cast<Size>(ustl::popcount(word));
        }
        return result;
    }

    [[nodiscard]]
    constexpr auto test_range(Size start, Size count) const noexcept -> bool {
        if (start > BitCount) {
            return false;
        }
        if (count > BitCount - start) {
            return false;
        }
        for (Size index = start; index < start + count; ++index) {
            if (!test(index)) {
                return false;
            }
        }
        return true;
    }

    template <typename Function>
    constexpr auto for_each(Function &&function) const -> void {
        for (Size word_index = 0; word_index < WORD_COUNT; ++word_index) {
            auto word = m_words[word_index];
            while (word != 0) {
                auto const bit = static_cast<Size>(ustl::countr_zero(word));
                auto const index = word_index * BITS_PER_WORD + bit;
                function(static_cast<Index>(index));
                word &= word - Word{1};
            }
        }
    }

    constexpr auto operator&=(BitSet const &other) noexcept -> BitSet & {
        for (Size index = 0; index < WORD_COUNT; ++index) {
            m_words[index] &= other.m_words[index];
        }
        return *this;
    }

    constexpr auto operator|=(BitSet const &other) noexcept -> BitSet & {
        for (Size index = 0; index < WORD_COUNT; ++index) {
            m_words[index] |= other.m_words[index];
        }
        return *this;
    }

    constexpr auto operator^=(BitSet const &other) noexcept -> BitSet & {
        for (Size index = 0; index < WORD_COUNT; ++index) {
            m_words[index] ^= other.m_words[index];
        }
        trim();
        return *this;
    }

    [[nodiscard]]
    friend constexpr auto operator~(BitSet value) noexcept -> BitSet {
        return value.flip();
    }

    [[nodiscard]]
    friend constexpr auto operator&(BitSet left, BitSet const &right) noexcept -> BitSet {
        return left &= right;
    }

    [[nodiscard]]
    friend constexpr auto operator|(BitSet left, BitSet const &right) noexcept -> BitSet {
        return left |= right;
    }

    [[nodiscard]]
    friend constexpr auto operator^(BitSet left, BitSet const &right) noexcept -> BitSet {
        return left ^= right;
    }

    [[nodiscard]]
    friend constexpr auto operator==(BitSet const &, BitSet const &) noexcept -> bool = default;

  private:
    [[nodiscard]]
    static constexpr auto in_range(Size index) noexcept -> bool {
        return index < BitCount;
    }

    [[nodiscard]]
    static constexpr auto word_index(Size index) noexcept -> Size {
        return index / BITS_PER_WORD;
    }

    [[nodiscard]]
    static constexpr auto bit_mask(Size index) noexcept -> Word {
        return Word{1} << (index % BITS_PER_WORD);
    }

    [[nodiscard]]
    static constexpr auto last_word_mask() noexcept -> Word {
        if constexpr (BitCount == 0) {
            return Word{0};
        }
        constexpr auto const REMAINDER = BitCount % BITS_PER_WORD;
        if constexpr (REMAINDER == 0) {
            return ~Word{0};
        }
        return (Word{1} << REMAINDER) - Word{1};
    }

    constexpr auto trim() noexcept -> void {
        m_words[WORD_COUNT - 1] &= last_word_mask();
    }

    std::array<Word, WORD_COUNT> m_words{};
};
} // namespace ustl

#endif // USTL_BITSET_HPP
