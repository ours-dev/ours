/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_SYSV_HASH_HPP
#define EFTL_ELF_SYSV_HASH_HPP 1

#include <eftl/elf/encoding.hpp>
#include <eftl/elf/symbol.hpp>
#include <eftl/elf/table.hpp>

#include <limits>

#include <cstring>

#include <ustl/option.hpp>

namespace eftl::elf {
template <typename Traits>
class SysvHashTableView final {
  public:
    using Word = u32;
    using Words = TableView<Word>;
    using Symbols = SymbolTableView<Traits>;

    [[nodiscard]]
    static auto from(ByteRange entries, Symbols symbols,
                     ByteOrder byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<SysvHashTableView> {
        if (entries.size() % sizeof(Word) != 0) {
            return ustl::NONE;
        }
        auto decoder = &ScalarCodec<Word>::decode_little;
        if (byte_order != ByteOrder::Little) {
            decoder = &ScalarCodec<Word>::decode_big;
        }
        auto const words =
            Words::from_checked_bytes(entries, entries.size() / sizeof(Word), decoder);
        if (words.is_none() || words->size() < K_HEADER_WORDS) {
            return ustl::NONE;
        }

        auto const bucket_count = static_cast<usize>((*words)[0]);
        auto const chain_count = static_cast<usize>((*words)[1]);
        if (bucket_count == 0 ||
            bucket_count > std::numeric_limits<usize>::max() - K_HEADER_WORDS ||
            chain_count > std::numeric_limits<usize>::max() - K_HEADER_WORDS - bucket_count ||
            words->size() != K_HEADER_WORDS + bucket_count + chain_count ||
            chain_count != symbols.size()) {
            return ustl::NONE;
        }

        auto const table = SysvHashTableView(words.value(), symbols, bucket_count, chain_count);
        if (table.valid()) {
            return ustl::some(SysvHashTableView{table});
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    constexpr auto bucket_count() const noexcept -> usize {
        return m_bucket_count;
    }

    [[nodiscard]]
    constexpr auto chain_count() const noexcept -> usize {
        return m_chain_count;
    }

    [[nodiscard]]
    auto bucket_at(usize index) const noexcept -> ustl::Option<Word> {
        if (index < m_bucket_count) {
            return m_words.at(K_HEADER_WORDS + index);
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    auto chain_at(usize symbol_index) const noexcept -> ustl::Option<Word> {
        if (symbol_index < m_chain_count) {
            return m_words.at(K_HEADER_WORDS + m_bucket_count + symbol_index);
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    static auto hash(ByteRange name) noexcept -> ustl::Option<Word> {
        if (!name.empty() && name.data() == nullptr) {
            return ustl::NONE;
        }

        Word value = 0;
        for (usize index = 0; index < name.size(); ++index) {
            value = static_cast<Word>((value << 4U) + name.data()[index]);
            auto const high_bits = value & 0xf0000000U;
            if (high_bits != 0) {
                value ^= high_bits >> 24U;
            }
            value &= ~high_bits;
        }
        return ustl::some(Word{value});
    }

    [[nodiscard]]
    auto lookup(ByteRange name) const noexcept -> ustl::Option<usize> {
        if (name.empty()) {
            return ustl::NONE;
        }
        auto const hash_value = hash(name);
        if (hash_value.is_none()) {
            return ustl::NONE;
        }
        auto const bucket = bucket_at(hash_value.value() % m_bucket_count);
        if (bucket.is_none()) {
            return ustl::NONE;
        }

        auto symbol_index = static_cast<usize>(bucket.value());
        for (usize steps = 0; symbol_index != 0 && steps < m_chain_count; ++steps) {
            auto const symbol_name = m_symbols.symbol_name(symbol_index);
            if (symbol_name.is_some() && equals(symbol_name.value(), name)) {
                return ustl::some(usize{symbol_index});
            }
            auto const next = chain_at(symbol_index);
            if (next.is_none()) {
                return ustl::NONE;
            }
            symbol_index = static_cast<usize>(next.value());
        }
        return ustl::NONE;
    }

  private:
    static constexpr usize K_HEADER_WORDS = 2;

    constexpr SysvHashTableView(Words words, Symbols symbols, usize bucket_count,
                                usize chain_count) noexcept
        : m_words(words)
        , m_symbols(symbols)
        , m_bucket_count(bucket_count)
        , m_chain_count(chain_count) {}

    [[nodiscard]]
    static auto equals(ByteRange lhs, ByteRange rhs) noexcept -> bool {
        return lhs.size() == rhs.size() &&
               (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0);
    }

    [[nodiscard]]
    auto valid() const noexcept -> bool {
        for (usize index = 0; index < m_bucket_count; ++index) {
            auto const bucket = bucket_at(index);
            if (bucket.is_none() || static_cast<usize>(bucket.value()) >= m_chain_count) {
                return false;
            }
        }
        for (usize index = 0; index < m_chain_count; ++index) {
            auto const chain = chain_at(index);
            if (chain.is_none() || static_cast<usize>(chain.value()) >= m_chain_count) {
                return false;
            }
        }

        usize traversed_symbols = 0;
        for (usize index = 0; index < m_bucket_count; ++index) {
            auto const bucket = bucket_at(index);
            auto symbol_index = static_cast<usize>(bucket.value());
            for (usize steps = 0; symbol_index != 0; ++steps) {
                if (steps >= m_chain_count || traversed_symbols >= m_chain_count) {
                    return false;
                }
                ++traversed_symbols;
                auto const symbol_name = m_symbols.symbol_name(symbol_index);
                if (symbol_name.is_none()) {
                    return false;
                }
                auto const symbol_hash = hash(symbol_name.value());
                if (symbol_hash.is_none() || symbol_hash.value() % m_bucket_count != index) {
                    return false;
                }
                auto const next = chain_at(symbol_index);
                if (next.is_none()) {
                    return false;
                }
                symbol_index = static_cast<usize>(next.value());
            }
        }
        return true;
    }

    Words m_words;
    Symbols m_symbols;
    usize m_bucket_count{0};
    usize m_chain_count{0};
};
} // namespace eftl::elf

#endif // EFTL_ELF_SYSV_HASH_HPP
