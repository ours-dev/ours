/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_SYMBOL_HPP
#define EFTL_ELF_SYMBOL_HPP 1

#include <eftl/elf/encoding.hpp>
#include <eftl/elf/section.hpp>
#include <eftl/elf/table.hpp>
#include <eftl/elf/traits.hpp>

#include <ustl/option.hpp>

namespace eftl::elf {
template <typename Traits>
class SymbolTableView final {
  public:
    using Symbol = typename Traits::Symbol;
    using Symbols = TableView<Symbol>;
    using ExtendedSectionIndex = u32;
    using ExtendedSectionIndices = TableView<ExtendedSectionIndex>;

    [[nodiscard]]
    static auto from(ByteRange const entries, ByteRange const strings,
                     ByteOrder const byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<SymbolTableView> {
        return from(entries, strings, ustl::NONE, byte_order);
    }

    [[nodiscard]]
    static auto from(ByteRange const entries, ByteRange const strings,
                     ustl::Option<ByteRange> const &extended_indices,
                     ByteOrder const byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<SymbolTableView> {
        if (entries.size() % sizeof(Symbol) != 0) {
            return ustl::NONE;
        }
        typename Symbols::Decoder symbol_decoder;
        if (byte_order == ByteOrder::Little) {
            symbol_decoder = &RecordCodec<Traits, Symbol>::decode_little;
        } else {
            symbol_decoder = &RecordCodec<Traits, Symbol>::decode_big;
        }
        auto const symbols =
            Symbols::from_checked_bytes(entries, entries.size() / sizeof(Symbol), symbol_decoder);
        auto const names = StringTableView::from(strings);
        if (symbols.is_none() || names.is_none()) {
            return ustl::NONE;
        }

        ustl::Option<ExtendedSectionIndices> indices;
        if (extended_indices.is_some()) {
            auto const bytes = extended_indices.value();
            if (bytes.size() % sizeof(ExtendedSectionIndex) != 0) {
                return ustl::NONE;
            }
            typename ExtendedSectionIndices::Decoder index_decoder;
            if (byte_order == ByteOrder::Little) {
                index_decoder = &ScalarCodec<ExtendedSectionIndex>::decode_little;
            } else {
                index_decoder = &ScalarCodec<ExtendedSectionIndex>::decode_big;
            }
            auto const table = ExtendedSectionIndices::from_checked_bytes(
                bytes, bytes.size() / sizeof(ExtendedSectionIndex), index_decoder);
            if (table.is_none() || table->size() != symbols->size()) {
                return ustl::NONE;
            }
            indices = table.value();
        }
        return ustl::some(SymbolTableView(symbols.value(), names.value(), indices));
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_symbols.size();
    }

    [[nodiscard]]
    auto symbol_at(usize const index) const noexcept -> ustl::Option<Symbol> {
        return m_symbols.at(index);
    }

    [[nodiscard]]
    auto symbol_name(usize const index) const noexcept -> ustl::Option<ByteRange> {
        auto const symbol = symbol_at(index);
        if (symbol.is_none()) {
            return ustl::NONE;
        }
        return m_names.string_at(static_cast<usize>(symbol->st_name));
    }

    [[nodiscard]]
    auto section_index(usize const index) const noexcept -> ustl::Option<usize> {
        auto const symbol = symbol_at(index);
        if (symbol.is_none()) {
            return ustl::NONE;
        }
        if (symbol->st_shndx != SECTION_INDEX_ESCAPE) {
            return ustl::some(static_cast<usize>(symbol->st_shndx));
        }
        if (m_extended_indices.is_none()) {
            return ustl::NONE;
        }
        auto const section_index = m_extended_indices->at(index);
        if (section_index.is_none()) {
            return ustl::NONE;
        }
        return ustl::some(static_cast<usize>(section_index.value()));
    }

    [[nodiscard]]
    static constexpr auto binding(Symbol const &symbol) noexcept -> u8 {
        return static_cast<u8>(symbol.st_info >> 4U);
    }

    [[nodiscard]]
    static constexpr auto type(Symbol const &symbol) noexcept -> u8 {
        return static_cast<u8>(symbol.st_info & 0x0fU);
    }

    [[nodiscard]]
    static constexpr auto visibility(Symbol const &symbol) noexcept -> u8 {
        return static_cast<u8>(symbol.st_other & 0x07U);
    }

  private:
    static constexpr auto SECTION_INDEX_ESCAPE = static_cast<typename Traits::Half>(0xffffU);

    constexpr SymbolTableView(Symbols const symbols, StringTableView const names,
                              ustl::Option<ExtendedSectionIndices> const extended_indices) noexcept
        : m_symbols(symbols), m_names(names), m_extended_indices(extended_indices) {}

    Symbols m_symbols;
    StringTableView m_names;
    ustl::Option<ExtendedSectionIndices> m_extended_indices;
};
} // namespace eftl::elf

#endif // EFTL_ELF_SYMBOL_HPP
