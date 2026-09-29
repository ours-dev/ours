/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_GNU_HASH_HPP
#define EFTL_ELF_GNU_HASH_HPP 1

#include <eftl/elf/encoding.hpp>
#include <eftl/elf/symbol.hpp>
#include <eftl/elf/table.hpp>

#include <limits>

#include <cstring>

#include <ustl/option.hpp>

namespace eftl::elf {
template <typename Traits>
class GnuHashTableView final {
  public:
    using Word = u32;
    using BloomWord = typename Traits::Addr;
    using Words = TableView<Word>;
    using Bloom = TableView<BloomWord>;
    using Symbols = SymbolTableView<Traits>;

    [[nodiscard]]
    static auto from(ByteRange entries, Symbols symbols,
                     ByteOrder byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<GnuHashTableView> {
        if (entries.size() < HEADER_BYTES || entries.data() == nullptr) {
            return ustl::NONE;
        }
        auto word_decoder = &ScalarCodec<Word>::decode_little;
        auto bloom_decoder = &ScalarCodec<BloomWord>::decode_little;
        if (byte_order != ByteOrder::Little) {
            word_decoder = &ScalarCodec<Word>::decode_big;
            bloom_decoder = &ScalarCodec<BloomWord>::decode_big;
        }
        auto const header = Words::from_checked_bytes(ByteRange(entries.data(), HEADER_BYTES),
                                                      HEADER_WORDS, word_decoder);
        if (header.is_none()) {
            return ustl::NONE;
        }

        auto const bucket_count = static_cast<usize>((*header)[0]);
        auto const symbol_offset = static_cast<usize>((*header)[1]);
        auto const bloom_count = static_cast<usize>((*header)[2]);
        auto const bloom_shift = (*header)[3];
        if (bucket_count == 0 || bloom_count == 0 || (bloom_count & (bloom_count - 1)) != 0 ||
            bloom_shift >= std::numeric_limits<Word>::digits || symbol_offset > symbols.size()) {
            return ustl::NONE;
        }

        auto size = HEADER_BYTES;
        if (!extend_size(size, bloom_count, sizeof(BloomWord)) ||
            !extend_size(size, bucket_count, sizeof(Word)) ||
            !extend_size(size, symbols.size() - symbol_offset, sizeof(Word)) ||
            size != entries.size()) {
            return ustl::NONE;
        }

        auto const bloom_bytes = bloom_count * sizeof(BloomWord);
        auto const bucket_bytes = bucket_count * sizeof(Word);
        auto const bloom = Bloom::from_checked_bytes(
            ByteRange(entries.data() + HEADER_BYTES, bloom_bytes), bloom_count, bloom_decoder);
        auto const buckets = Words::from_checked_bytes(
            ByteRange(entries.data() + HEADER_BYTES + bloom_bytes, bucket_bytes), bucket_count,
            word_decoder);
        auto const chains = Words::from_checked_bytes(
            ByteRange(entries.data() + HEADER_BYTES + bloom_bytes + bucket_bytes,
                      entries.size() - HEADER_BYTES - bloom_bytes - bucket_bytes),
            symbols.size() - symbol_offset, word_decoder);
        if (bloom.is_none() || buckets.is_none() || chains.is_none()) {
            return ustl::NONE;
        }

        auto const table = GnuHashTableView(bloom.value(), buckets.value(), chains.value(), symbols,
                                            bucket_count, symbol_offset, bloom_count, bloom_shift);
        if (table.valid()) {
            return ustl::some(GnuHashTableView{table});
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    constexpr auto bucket_count() const noexcept -> usize {
        return m_bucket_count;
    }

    [[nodiscard]]
    constexpr auto symbol_offset() const noexcept -> usize {
        return m_symbol_offset;
    }

    [[nodiscard]]
    constexpr auto bloom_count() const noexcept -> usize {
        return m_bloom_count;
    }

    [[nodiscard]]
    constexpr auto bloom_shift() const noexcept -> Word {
        return m_bloom_shift;
    }

    [[nodiscard]]
    constexpr auto chain_count() const noexcept -> usize {
        return m_chains.size();
    }

    [[nodiscard]]
    auto bloom_at(usize index) const noexcept -> ustl::Option<BloomWord> {
        return m_bloom.at(index);
    }

    [[nodiscard]]
    auto bucket_at(usize index) const noexcept -> ustl::Option<Word> {
        return m_buckets.at(index);
    }

    [[nodiscard]]
    auto chain_at(usize symbol_index) const noexcept -> ustl::Option<Word> {
        if (symbol_index < m_symbol_offset) {
            return ustl::NONE;
        }
        return m_chains.at(symbol_index - m_symbol_offset);
    }

    [[nodiscard]]
    static auto hash(ByteRange name) noexcept -> ustl::Option<Word> {
        if (!name.empty() && name.data() == nullptr) {
            return ustl::NONE;
        }

        Word value = 5381;
        for (usize index = 0; index < name.size(); ++index) {
            value = static_cast<Word>(value * 33U + name.data()[index]);
        }
        return ustl::some(Word{value});
    }

    [[nodiscard]]
    auto maybe_contains(ByteRange name) const noexcept -> ustl::Option<bool> {
        if (name.empty()) {
            return ustl::some(false);
        }
        auto const value = hash(name);
        if (value.is_none()) {
            return ustl::NONE;
        }
        return ustl::some(bool{maybe_contains_hash(value.value())});
    }

    [[nodiscard]]
    auto lookup(ByteRange name) const noexcept -> ustl::Option<usize> {
        if (name.empty()) {
            return ustl::NONE;
        }
        auto const hash_value = hash(name);
        if (hash_value.is_none() || !maybe_contains_hash(hash_value.value())) {
            return ustl::NONE;
        }

        auto const bucket = bucket_at(hash_value.value() % m_bucket_count);
        if (bucket.is_none() || bucket.value() == 0) {
            return ustl::NONE;
        }

        auto symbol_index = static_cast<usize>(bucket.value());
        for (usize steps = 0; steps < chain_count(); ++steps) {
            auto const chain = chain_at(symbol_index);
            if (chain.is_none()) {
                return ustl::NONE;
            }
            if (((chain.value() ^ hash_value.value()) >> 1U) == 0) {
                auto const symbol_name = m_symbols.symbol_name(symbol_index);
                if (symbol_name.is_some() && equals(symbol_name.value(), name)) {
                    return ustl::some(usize{symbol_index});
                }
            }
            if ((chain.value() & 1U) != 0) {
                return ustl::NONE;
            }
            if (symbol_index == std::numeric_limits<usize>::max()) {
                return ustl::NONE;
            }
            ++symbol_index;
        }
        return ustl::NONE;
    }

  private:
    static constexpr usize HEADER_WORDS = 4;
    static constexpr usize HEADER_BYTES = HEADER_WORDS * sizeof(Word);
    static constexpr usize BLOOM_WORD_BITS = std::numeric_limits<BloomWord>::digits;

    constexpr GnuHashTableView(Bloom bloom, Words buckets, Words chains, Symbols symbols,
                               usize bucket_count, usize symbol_offset, usize bloom_count,
                               Word bloom_shift) noexcept
        : m_bloom(bloom)
        , m_buckets(buckets)
        , m_chains(chains)
        , m_symbols(symbols)
        , m_bucket_count(bucket_count)
        , m_symbol_offset(symbol_offset)
        , m_bloom_count(bloom_count)
        , m_bloom_shift(bloom_shift) {}

    [[nodiscard]]
    static constexpr auto extend_size(usize &size, usize count, usize element_size) noexcept
        -> bool {
        if (count > std::numeric_limits<usize>::max() / element_size) {
            return false;
        }
        auto const bytes = count * element_size;
        if (size > std::numeric_limits<usize>::max() - bytes) {
            return false;
        }
        size += bytes;
        return true;
    }

    [[nodiscard]]
    static auto equals(ByteRange lhs, ByteRange rhs) noexcept -> bool {
        return lhs.size() == rhs.size() &&
               (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0);
    }

    [[nodiscard]]
    auto maybe_contains_hash(Word value) const noexcept -> bool {
        auto const bloom_index =
            (static_cast<usize>(value) / BLOOM_WORD_BITS) & (m_bloom_count - 1);
        auto const bloom_word = bloom_at(bloom_index);
        if (bloom_word.is_none()) {
            return false;
        }
        auto const first_bit = static_cast<usize>(value) % BLOOM_WORD_BITS;
        auto const second_bit = static_cast<usize>(value >> m_bloom_shift) % BLOOM_WORD_BITS;
        auto const mask =
            static_cast<BloomWord>((BloomWord{1} << first_bit) | (BloomWord{1} << second_bit));
        return (bloom_word.value() & mask) == mask;
    }

    [[nodiscard]]
    auto valid() const noexcept -> bool {
        usize nonempty_buckets = 0;
        for (usize index = 0; index < m_bucket_count; ++index) {
            auto const bucket = bucket_at(index);
            if (bucket.is_none()) {
                return false;
            }
            if (bucket.value() == 0) {
                continue;
            }
            auto const symbol_index = static_cast<usize>(bucket.value());
            if (symbol_index < m_symbol_offset || symbol_index >= m_symbols.size()) {
                return false;
            }
            ++nonempty_buckets;
        }

        usize symbol_index = m_symbol_offset;
        usize chain_count = 0;
        while (symbol_index < m_symbols.size()) {
            auto const first_name = m_symbols.symbol_name(symbol_index);
            if (first_name.is_none()) {
                return false;
            }
            auto const first_hash = hash(first_name.value());
            if (first_hash.is_none()) {
                return false;
            }
            auto const expected_bucket = static_cast<usize>(first_hash.value() % m_bucket_count);
            auto const bucket = bucket_at(expected_bucket);
            if (bucket.is_none() || static_cast<usize>(bucket.value()) != symbol_index) {
                return false;
            }
            ++chain_count;

            while (true) {
                auto const symbol_name = m_symbols.symbol_name(symbol_index);
                if (symbol_name.is_none()) {
                    return false;
                }
                auto const symbol_hash = hash(symbol_name.value());
                auto const chain = chain_at(symbol_index);
                if (symbol_hash.is_none() || chain.is_none() ||
                    symbol_hash.value() % m_bucket_count != expected_bucket ||
                    ((symbol_hash.value() ^ chain.value()) >> 1U) != 0 ||
                    !maybe_contains_hash(symbol_hash.value())) {
                    return false;
                }
                if ((chain.value() & 1U) != 0) {
                    ++symbol_index;
                    break;
                }
                if (symbol_index + 1 >= m_symbols.size()) {
                    return false;
                }
                ++symbol_index;
            }
        }
        return chain_count == nonempty_buckets;
    }

    Bloom m_bloom;
    Words m_buckets;
    Words m_chains;
    Symbols m_symbols;
    usize m_bucket_count{0};
    usize m_symbol_offset{0};
    usize m_bloom_count{0};
    Word m_bloom_shift{0};
};
} // namespace eftl::elf

#endif // EFTL_ELF_GNU_HASH_HPP
