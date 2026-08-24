/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/symbol.hpp>

#include <array>

#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Section = Traits::Section;
using Symbol = Traits::Symbol;

constexpr usize K_SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize K_STRING_TABLE_OFFSET = 0x200;
constexpr usize K_SYMTAB_OFFSET = 0x240;
constexpr usize K_DYNSYM_OFFSET = 0x280;
constexpr usize K_SYMTAB_SHNDX_OFFSET = 0x2c0;

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

auto make_header() -> Header {
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 1;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_shoff = K_SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 5;
    header.e_shstrndx = 0;
    return header;
}

auto make_symbol_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    auto const header = make_header();
    put(image, 0, header);

    Section strings{};
    strings.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_TABLE_OFFSET;
    strings.sh_size = 18;
    strings.sh_addralign = 1;

    Section symtab{};
    symtab.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Symtab);
    symtab.sh_offset = K_SYMTAB_OFFSET;
    symtab.sh_size = 2 * sizeof(Symbol);
    symtab.sh_link = 1;
    symtab.sh_info = 1;
    symtab.sh_addralign = alignof(Symbol);
    symtab.sh_entsize = sizeof(Symbol);

    Section dynsym = symtab;
    dynsym.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Dynsym);
    dynsym.sh_offset = K_DYNSYM_OFFSET;

    Section symtab_shndx{};
    symtab_shndx.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::SymtabShndx);
    symtab_shndx.sh_offset = K_SYMTAB_SHNDX_OFFSET;
    symtab_shndx.sh_size = 2 * sizeof(ours::u32);
    symtab_shndx.sh_link = 2;
    symtab_shndx.sh_addralign = alignof(ours::u32);
    symtab_shndx.sh_entsize = sizeof(ours::u32);

    put(image, K_SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), symtab);
    put(image, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), dynsym);
    put(image, K_SECTION_TABLE_OFFSET + 4 * sizeof(Section), symtab_shndx);

    std::array<u8, 18> const names{
        0, 'l', 'o', 'c', 'a', 'l', 0, 'g', 'l', 'o', 'b', 'a', 'l', 0, 'd', 'y', 'n', 0,
    };
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());

    Symbol named{};
    named.st_name = 7;
    named.st_info = static_cast<u8>((1U << 4U) | 2U);
    named.st_other = 7;
    named.st_shndx = eftl::elf::K_SHN_XINDEX;
    named.st_value = 0x400000;
    named.st_size = 4;
    put(image, K_SYMTAB_OFFSET + sizeof(Symbol), named);

    named.st_name = 14;
    named.st_shndx = 1;
    put(image, K_DYNSYM_OFFSET + sizeof(Symbol), named);

    std::array<ours::u32, 2> const section_indices{0, 1};
    std::memcpy(image.data() + K_SYMTAB_SHNDX_OFFSET, section_indices.data(),
                sizeof(section_indices));
    return image;
}

auto test_observes_symbol_records_and_names() -> int {
    auto const image = make_symbol_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    auto const table = parsed->symbol_table(2);
    ustl::Option<Symbol> symbol = ustl::NONE;
    if (table.is_some()) {
        symbol = table->symbol_at(1);
    }
    ustl::Option<eftl::ByteRange> name = ustl::NONE;
    if (table.is_some()) {
        name = table->symbol_name(1);
    }
    ustl::Option<usize> section_index = ustl::NONE;
    if (table.is_some()) {
        section_index = table->section_index(1);
    }
    if (symbol.is_none() || name.is_none() || name->size() != 6 || name->data()[0] != 'g' ||
        eftl::elf::SymbolTableView<Traits>::binding(symbol.value()) != 1 ||
        eftl::elf::SymbolTableView<Traits>::type(symbol.value()) != 2 ||
        eftl::elf::SymbolTableView<Traits>::visibility(symbol.value()) != 7 ||
        section_index.is_none() || section_index.value() != 1 || table->symbol_at(2).is_some()) {
        return 2;
    }
    auto const dynamic_table = parsed->symbol_table(3);
    ustl::Option<eftl::ByteRange> dynamic_name = ustl::NONE;
    if (dynamic_table.is_some()) {
        dynamic_name = dynamic_table->symbol_name(1);
    }
    ustl::Option<usize> dynamic_section_index = ustl::NONE;
    if (dynamic_table.is_some()) {
        dynamic_section_index = dynamic_table->section_index(1);
    }
    if (dynamic_name.is_some() && dynamic_name->size() == 3 && dynamic_name->data()[0] == 'd' &&
        dynamic_section_index.is_some() && dynamic_section_index.value() == 1) {
        return 0;
    }
    return 3;
}

auto test_symbol_name_is_bounded_per_record() -> int {
    auto image = make_symbol_image();
    Symbol symbol{};
    std::memcpy(&symbol, image.data() + K_SYMTAB_OFFSET + sizeof(Symbol), sizeof(symbol));
    symbol.st_name = 18;
    put(image, K_SYMTAB_OFFSET + sizeof(Symbol), symbol);
    auto const parsed = Binary::from_raw(image.data(), image.size());
    ustl::Option<eftl::elf::SymbolTableView<Traits>> table = ustl::NONE;
    if (parsed.is_ok()) {
        table = parsed->symbol_table(2);
    }
    if (table.is_some() && table->symbol_name(1).is_none()) {
        return 0;
    }
    return 1;
}

auto test_symbol_view_rejects_mismatched_section_index_table() -> int {
    auto const image = make_symbol_image();
    auto const symbols = eftl::ByteRange(image.data() + K_SYMTAB_OFFSET, 2 * sizeof(Symbol));
    auto const strings = eftl::ByteRange(image.data() + K_STRING_TABLE_OFFSET, 18);
    auto const indices = eftl::ByteRange(image.data() + K_SYMTAB_SHNDX_OFFSET, sizeof(ours::u32));
    if (eftl::elf::SymbolTableView<Traits>::from(symbols, strings, indices).is_none()) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_symbol_section_relationships() -> int {
    auto wrong_entry_size = make_symbol_image();
    Section section{};
    std::memcpy(&section, wrong_entry_size.data() + K_SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(section));
    section.sh_entsize = 2 * sizeof(Symbol);
    put(wrong_entry_size, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), section);
    auto const wrong_entry_result =
        Binary::from_raw(wrong_entry_size.data(), wrong_entry_size.size());
    if (wrong_entry_result.is_ok() ||
        wrong_entry_result.unwrap_err() != eftl::elf::ParseError::InvalidSymbol) {
        return 1;
    }

    auto wrong_link = make_symbol_image();
    std::memcpy(&section, wrong_link.data() + K_SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(section));
    section.sh_link = 2;
    put(wrong_link, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), section);
    auto const wrong_link_result = Binary::from_raw(wrong_link.data(), wrong_link.size());
    if (wrong_link_result.is_ok() ||
        wrong_link_result.unwrap_err() != eftl::elf::ParseError::InvalidSymbol) {
        return 2;
    }

    auto too_many_locals = make_symbol_image();
    std::memcpy(&section, too_many_locals.data() + K_SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(section));
    section.sh_info = 3;
    put(too_many_locals, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), section);
    auto const too_many_locals_result =
        Binary::from_raw(too_many_locals.data(), too_many_locals.size());
    if (too_many_locals_result.is_ok() ||
        too_many_locals_result.unwrap_err() != eftl::elf::ParseError::InvalidSymbol) {
        return 3;
    }

    auto wrong_shndx_entry_size = make_symbol_image();
    std::memcpy(&section,
                wrong_shndx_entry_size.data() + K_SECTION_TABLE_OFFSET + 4 * sizeof(Section),
                sizeof(section));
    section.sh_entsize = sizeof(u16);
    put(wrong_shndx_entry_size, K_SECTION_TABLE_OFFSET + 4 * sizeof(Section), section);
    auto const wrong_shndx_entry_size_result =
        Binary::from_raw(wrong_shndx_entry_size.data(), wrong_shndx_entry_size.size());
    if (wrong_shndx_entry_size_result.is_ok() ||
        wrong_shndx_entry_size_result.unwrap_err() != eftl::elf::ParseError::InvalidSymbol) {
        return 4;
    }

    auto nonzero_shndx_info = make_symbol_image();
    std::memcpy(&section, nonzero_shndx_info.data() + K_SECTION_TABLE_OFFSET + 4 * sizeof(Section),
                sizeof(section));
    section.sh_info = 1;
    put(nonzero_shndx_info, K_SECTION_TABLE_OFFSET + 4 * sizeof(Section), section);
    auto const nonzero_shndx_info_result =
        Binary::from_raw(nonzero_shndx_info.data(), nonzero_shndx_info.size());
    if (nonzero_shndx_info_result.is_ok() ||
        nonzero_shndx_info_result.unwrap_err() != eftl::elf::ParseError::InvalidSymbol) {
        return 5;
    }

    auto wrong_shndx_link = make_symbol_image();
    std::memcpy(&section, wrong_shndx_link.data() + K_SECTION_TABLE_OFFSET + 4 * sizeof(Section),
                sizeof(section));
    section.sh_link = 1;
    put(wrong_shndx_link, K_SECTION_TABLE_OFFSET + 4 * sizeof(Section), section);
    auto const wrong_shndx_link_result =
        Binary::from_raw(wrong_shndx_link.data(), wrong_shndx_link.size());
    if (wrong_shndx_link_result.is_ok() ||
        wrong_shndx_link_result.unwrap_err() != eftl::elf::ParseError::InvalidSymbol) {
        return 6;
    }

    auto duplicate_shndx = make_symbol_image();
    Header header{};
    std::memcpy(&header, duplicate_shndx.data(), sizeof(header));
    header.e_shnum = 6;
    put(duplicate_shndx, 0, header);
    std::memcpy(&section, duplicate_shndx.data() + K_SECTION_TABLE_OFFSET + 4 * sizeof(Section),
                sizeof(section));
    put(duplicate_shndx, K_SECTION_TABLE_OFFSET + 5 * sizeof(Section), section);
    auto const duplicate_shndx_result =
        Binary::from_raw(duplicate_shndx.data(), duplicate_shndx.size());
    if (duplicate_shndx_result.is_ok() ||
        duplicate_shndx_result.unwrap_err() != eftl::elf::ParseError::InvalidSymbol) {
        return 7;
    }

    auto ordinary_symbol_shndx = make_symbol_image();
    const ours::u32 nonzero_index = 1;
    put(ordinary_symbol_shndx, K_SYMTAB_SHNDX_OFFSET, nonzero_index);
    auto const ordinary_symbol_shndx_result =
        Binary::from_raw(ordinary_symbol_shndx.data(), ordinary_symbol_shndx.size());
    if (ordinary_symbol_shndx_result.is_ok() ||
        ordinary_symbol_shndx_result.unwrap_err() != eftl::elf::ParseError::InvalidSymbol) {
        return 8;
    }

    auto out_of_range_shndx = make_symbol_image();
    const ours::u32 out_of_range_index = 5;
    put(out_of_range_shndx, K_SYMTAB_SHNDX_OFFSET + sizeof(ours::u32), out_of_range_index);
    auto const out_of_range_shndx_result =
        Binary::from_raw(out_of_range_shndx.data(), out_of_range_shndx.size());
    if (out_of_range_shndx_result.is_ok() ||
        out_of_range_shndx_result.unwrap_err() != eftl::elf::ParseError::InvalidSymbol) {
        return 9;
    }

    auto missing_shndx = make_symbol_image();
    std::memcpy(&section, missing_shndx.data() + K_SECTION_TABLE_OFFSET + 4 * sizeof(Section),
                sizeof(section));
    section.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Null);
    put(missing_shndx, K_SECTION_TABLE_OFFSET + 4 * sizeof(Section), section);
    auto const missing_shndx_result = Binary::from_raw(missing_shndx.data(), missing_shndx.size());
    if (missing_shndx_result.is_err() &&
        missing_shndx_result.unwrap_err() == eftl::elf::ParseError::InvalidSymbol) {
        return 0;
    }
    return 10;
}

auto test_elf32_observes_symbol_table() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Section32 = Traits32::Section;
    using Symbol32 = Traits32::Symbol;

    constexpr usize K_SECTION_OFFSET = sizeof(Header32);
    constexpr usize K_STRING_OFFSET = 0x100;
    constexpr usize K_SYMBOL_OFFSET = 0x120;
    constexpr usize K_SHNDX_OFFSET = 0x160;
    std::array<u8, 512> image{};
    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits32::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 1;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header32);
    header.e_shoff = K_SECTION_OFFSET;
    header.e_shentsize = sizeof(Section32);
    header.e_shnum = 4;
    put(image, 0, header);

    Section32 strings{};
    strings.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_OFFSET;
    strings.sh_size = 5;
    strings.sh_addralign = 1;
    Section32 symbols{};
    symbols.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Symtab);
    symbols.sh_offset = K_SYMBOL_OFFSET;
    symbols.sh_size = 2 * sizeof(Symbol32);
    symbols.sh_link = 1;
    symbols.sh_info = 1;
    symbols.sh_addralign = alignof(Symbol32);
    symbols.sh_entsize = sizeof(Symbol32);
    Section32 symbol_indices{};
    symbol_indices.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::SymtabShndx);
    symbol_indices.sh_offset = K_SHNDX_OFFSET;
    symbol_indices.sh_size = 2 * sizeof(ours::u32);
    symbol_indices.sh_link = 2;
    symbol_indices.sh_addralign = alignof(ours::u32);
    symbol_indices.sh_entsize = sizeof(ours::u32);
    put(image, K_SECTION_OFFSET + sizeof(Section32), strings);
    put(image, K_SECTION_OFFSET + 2 * sizeof(Section32), symbols);
    put(image, K_SECTION_OFFSET + 3 * sizeof(Section32), symbol_indices);
    std::array<u8, 5> const names{0, 'f', 'o', 'o', 0};
    std::memcpy(image.data() + K_STRING_OFFSET, names.data(), names.size());
    Symbol32 symbol{};
    symbol.st_name = 1;
    symbol.st_info = static_cast<u8>((2U << 4U) | 1U);
    symbol.st_other = 2;
    symbol.st_shndx = eftl::elf::K_SHN_XINDEX;
    put(image, K_SYMBOL_OFFSET + sizeof(Symbol32), symbol);
    std::array<ours::u32, 2> const section_indices{0, 1};
    std::memcpy(image.data() + K_SHNDX_OFFSET, section_indices.data(), sizeof(section_indices));

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    ustl::Option<eftl::elf::SymbolTableView<Traits32>> table = ustl::NONE;
    if (parsed.is_ok()) {
        table = parsed->symbol_table(2);
    }
    ustl::Option<eftl::ByteRange> name = ustl::NONE;
    if (table.is_some()) {
        name = table->symbol_name(1);
    }
    if (name.is_some() && name->size() == 3 &&
        eftl::elf::SymbolTableView<Traits32>::binding(table->symbol_at(1).value()) == 2 &&
        eftl::elf::SymbolTableView<Traits32>::type(table->symbol_at(1).value()) == 1 &&
        eftl::elf::SymbolTableView<Traits32>::visibility(table->symbol_at(1).value()) == 2 &&
        table->section_index(1).is_some() && table->section_index(1).value() == 1) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_symbol_records_and_names(); result != 0) {
        return result;
    }
    if (auto const result = test_symbol_name_is_bounded_per_record(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_symbol_view_rejects_mismatched_section_index_table();
        result != 0) {
        return 20 + result;
    }
    if (auto const result = test_rejects_invalid_symbol_section_relationships(); result != 0) {
        return 30 + result;
    }
    if (test_elf32_observes_symbol_table() == 0) {
        return 0;
    }
    return 40;
}
