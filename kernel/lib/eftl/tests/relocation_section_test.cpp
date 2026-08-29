/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/relocation_section.hpp>

#include <array>

#include <cstddef>
#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Rel = Traits::Rel;
using Rela = Traits::Rela;
using Section = Traits::Section;
using Symbol = Traits::Symbol;

constexpr usize K_SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize K_STRING_TABLE_OFFSET = 0x200;
constexpr usize K_SYMBOL_TABLE_OFFSET = 0x220;
constexpr usize K_REL_TABLE_OFFSET = 0x260;
constexpr usize K_RELA_TABLE_OFFSET = 0x280;

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

template <usize Size>
auto put_big_endian_u16(std::array<u8, Size> &image, usize offset, u16 value)
    -> void {
    image[offset] = static_cast<u8>(value >> 8U);
    image[offset + 1] = static_cast<u8>(value);
}

template <usize Size>
auto put_big_endian_u32(std::array<u8, Size> &image, usize offset, u32 value)
    -> void {
    for (usize index = 0; index < sizeof(value); ++index) {
        image[offset + index] = static_cast<u8>(
            value >> ((sizeof(value) - index - 1) * std::numeric_limits<u8>::digits));
    }
}

template <usize Size>
auto put_big_endian_u64(std::array<u8, Size> &image, usize offset, u64 value)
    -> void {
    for (usize index = 0; index < sizeof(value); ++index) {
        image[offset + index] = static_cast<u8>(
            value >> ((sizeof(value) - index - 1) * std::numeric_limits<u8>::digits));
    }
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
    return header;
}

auto make_relocation_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    auto const header = make_header();
    put(image, 0, header);

    Section strings{};
    strings.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_TABLE_OFFSET;
    strings.sh_size = 8;
    strings.sh_addralign = 1;

    Section symbols{};
    symbols.sh_type = static_cast<u32>(eftl::elf::SectionType::Symtab);
    symbols.sh_offset = K_SYMBOL_TABLE_OFFSET;
    symbols.sh_size = 2 * sizeof(Symbol);
    symbols.sh_link = 1;
    symbols.sh_info = 1;
    symbols.sh_addralign = alignof(Symbol);
    symbols.sh_entsize = sizeof(Symbol);

    Section rel{};
    rel.sh_type = static_cast<u32>(eftl::elf::SectionType::Rel);
    rel.sh_offset = K_REL_TABLE_OFFSET;
    rel.sh_size = sizeof(Rel);
    rel.sh_link = 2;
    rel.sh_addralign = alignof(Rel);
    rel.sh_entsize = sizeof(Rel);

    Section rela{};
    rela.sh_type = static_cast<u32>(eftl::elf::SectionType::Rela);
    rela.sh_offset = K_RELA_TABLE_OFFSET;
    rela.sh_size = sizeof(Rela);
    rela.sh_link = 2;
    rela.sh_addralign = alignof(Rela);
    rela.sh_entsize = sizeof(Rela);

    put(image, K_SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), symbols);
    put(image, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), rel);
    put(image, K_SECTION_TABLE_OFFSET + 4 * sizeof(Section), rela);

    std::array<u8, 8> const names{0, 't', 'a', 'r', 'g', 'e', 't', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());

    Symbol symbol{};
    symbol.st_name = 1;
    put(image, K_SYMBOL_TABLE_OFFSET + sizeof(Symbol), symbol);

    Rel relative{};
    relative.r_offset = 0x401000;
    relative.r_info = 8;
    put(image, K_REL_TABLE_OFFSET, relative);

    Rela named{};
    named.r_offset = 0x402000;
    named.r_info = (static_cast<u64>(1) << 32U) | 1U;
    named.r_addend = -16;
    put(image, K_RELA_TABLE_OFFSET, named);
    return image;
}

auto make_big_endian_relocation_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    std::memcpy(image.data(), eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    image[4] = Traits::K_ELF_CLASS;
    image[5] = 2;
    image[6] = 1;
    put_big_endian_u16(image, offsetof(Header, e_type), 1);
    put_big_endian_u16(image, offsetof(Header, e_machine),
                       static_cast<u16>(eftl::elf::Arch::X86_64));
    put_big_endian_u32(image, offsetof(Header, e_version), 1);
    put_big_endian_u16(image, offsetof(Header, e_ehsize), sizeof(Header));
    put_big_endian_u64(image, offsetof(Header, e_shoff), K_SECTION_TABLE_OFFSET);
    put_big_endian_u16(image, offsetof(Header, e_shentsize), sizeof(Section));
    put_big_endian_u16(image, offsetof(Header, e_shnum), 5);

    constexpr usize K_STRING_SECTION = K_SECTION_TABLE_OFFSET + sizeof(Section);
    constexpr usize K_SYMBOL_SECTION = K_SECTION_TABLE_OFFSET + 2 * sizeof(Section);
    constexpr usize K_REL_SECTION = K_SECTION_TABLE_OFFSET + 3 * sizeof(Section);
    constexpr usize K_RELA_SECTION = K_SECTION_TABLE_OFFSET + 4 * sizeof(Section);
    put_big_endian_u32(image, K_STRING_SECTION + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Strtab));
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_offset),
                       K_STRING_TABLE_OFFSET);
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_size), 8);
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_addralign), 1);

    put_big_endian_u32(image, K_SYMBOL_SECTION + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Symtab));
    put_big_endian_u64(image, K_SYMBOL_SECTION + offsetof(Section, sh_offset),
                       K_SYMBOL_TABLE_OFFSET);
    put_big_endian_u64(image, K_SYMBOL_SECTION + offsetof(Section, sh_size), 2 * sizeof(Symbol));
    put_big_endian_u32(image, K_SYMBOL_SECTION + offsetof(Section, sh_link), 1);
    put_big_endian_u32(image, K_SYMBOL_SECTION + offsetof(Section, sh_info), 1);
    put_big_endian_u64(image, K_SYMBOL_SECTION + offsetof(Section, sh_addralign), alignof(Symbol));
    put_big_endian_u64(image, K_SYMBOL_SECTION + offsetof(Section, sh_entsize), sizeof(Symbol));

    put_big_endian_u32(image, K_REL_SECTION + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Rel));
    put_big_endian_u64(image, K_REL_SECTION + offsetof(Section, sh_offset), K_REL_TABLE_OFFSET);
    put_big_endian_u64(image, K_REL_SECTION + offsetof(Section, sh_size), sizeof(Rel));
    put_big_endian_u32(image, K_REL_SECTION + offsetof(Section, sh_link), 2);
    put_big_endian_u64(image, K_REL_SECTION + offsetof(Section, sh_addralign), alignof(Rel));
    put_big_endian_u64(image, K_REL_SECTION + offsetof(Section, sh_entsize), sizeof(Rel));

    put_big_endian_u32(image, K_RELA_SECTION + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Rela));
    put_big_endian_u64(image, K_RELA_SECTION + offsetof(Section, sh_offset), K_RELA_TABLE_OFFSET);
    put_big_endian_u64(image, K_RELA_SECTION + offsetof(Section, sh_size), sizeof(Rela));
    put_big_endian_u32(image, K_RELA_SECTION + offsetof(Section, sh_link), 2);
    put_big_endian_u64(image, K_RELA_SECTION + offsetof(Section, sh_addralign), alignof(Rela));
    put_big_endian_u64(image, K_RELA_SECTION + offsetof(Section, sh_entsize), sizeof(Rela));

    std::array<u8, 8> const names{0, 't', 'a', 'r', 'g', 'e', 't', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());
    put_big_endian_u32(image, K_SYMBOL_TABLE_OFFSET + sizeof(Symbol), 1);
    put_big_endian_u64(image, K_REL_TABLE_OFFSET, 0x401000);
    put_big_endian_u64(image, K_REL_TABLE_OFFSET + sizeof(u64), 8);
    put_big_endian_u64(image, K_RELA_TABLE_OFFSET, 0x402000);
    put_big_endian_u64(image, K_RELA_TABLE_OFFSET + sizeof(u64),
                       (static_cast<u64>(1) << 32U) | 1U);
    put_big_endian_u64(image, K_RELA_TABLE_OFFSET + 2 * sizeof(u64),
                       static_cast<u64>(-16));
    return image;
}

auto test_observes_rel_and_rela_records() -> int {
    auto const image = make_relocation_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }

    auto const rel_table = parsed->rel_table(3);
    auto const rela_table = parsed->rela_table(4);
    ustl::Option<Rel> relative = ustl::NONE;
    if (rel_table.is_some()) {
        relative = rel_table->relocation_at(0);
    }
    ustl::Option<Rela> named = ustl::NONE;
    if (rela_table.is_some()) {
        named = rela_table->relocation_at(0);
    }
    if (relative.is_none() || named.is_none() || relative->r_offset != 0x401000 ||
        named->r_offset != 0x402000 || named->r_addend != -16 || rel_table->size() != 1 ||
        rela_table->size() != 1 ||
        eftl::elf::RelocationTableView<Traits, Rel>::type(relative.value()) != 8 ||
        eftl::elf::RelocationTableView<Traits, Rel>::symbol_index(relative.value()) != 0 ||
        eftl::elf::RelocationTableView<Traits, Rela>::type(named.value()) != 1 ||
        eftl::elf::RelocationTableView<Traits, Rela>::symbol_index(named.value()) != 1) {
        return 2;
    }
    if (parsed->rel_table(4).is_none() && parsed->rela_table(3).is_none()) {
        return 0;
    }
    return 3;
}

auto test_observes_big_endian_rel_and_rela_records() -> int {
    auto const image = make_big_endian_relocation_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    ustl::Option<eftl::elf::RelocationTableView<Traits, Rel>> rel_table = ustl::NONE;
    if (parsed.is_ok()) {
        rel_table = parsed->rel_table(3);
    }
    ustl::Option<eftl::elf::RelocationTableView<Traits, Rela>> rela_table = ustl::NONE;
    if (parsed.is_ok()) {
        rela_table = parsed->rela_table(4);
    }
    ustl::Option<Rel> relative = ustl::NONE;
    if (rel_table.is_some()) {
        relative = rel_table->relocation_at(0);
    }
    ustl::Option<Rela> named = ustl::NONE;
    if (rela_table.is_some()) {
        named = rela_table->relocation_at(0);
    }
    if (relative.is_some() && named.is_some() && relative->r_offset == 0x401000 &&
        named->r_offset == 0x402000 && named->r_addend == -16 &&
        eftl::elf::RelocationTableView<Traits, Rel>::type(relative.value()) == 8 &&
        eftl::elf::RelocationTableView<Traits, Rela>::symbol_index(named.value()) == 1) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_relocation_section_relationships() -> int {
    auto wrong_entry_size = make_relocation_image();
    Section rel{};
    std::memcpy(&rel, wrong_entry_size.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(rel));
    rel.sh_size = 2 * sizeof(Rel);
    rel.sh_entsize = 2 * sizeof(Rel);
    put(wrong_entry_size, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), rel);
    auto const wrong_entry_size_result =
        Binary::from_raw(wrong_entry_size.data(), wrong_entry_size.size());
    if (wrong_entry_size_result.is_ok() ||
        wrong_entry_size_result.unwrap_err() != eftl::elf::ParseError::InvalidRelocation) {
        return 1;
    }

    auto wrong_link = make_relocation_image();
    std::memcpy(&rel, wrong_link.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(rel));
    rel.sh_link = 1;
    put(wrong_link, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), rel);
    auto const wrong_link_result = Binary::from_raw(wrong_link.data(), wrong_link.size());
    if (wrong_link_result.is_ok() ||
        wrong_link_result.unwrap_err() != eftl::elf::ParseError::InvalidRelocation) {
        return 2;
    }

    auto out_of_range_symbol = make_relocation_image();
    Rel relocation{};
    std::memcpy(&relocation, out_of_range_symbol.data() + K_REL_TABLE_OFFSET, sizeof(relocation));
    relocation.r_info = static_cast<u64>(2) << 32U;
    put(out_of_range_symbol, K_REL_TABLE_OFFSET, relocation);
    auto const out_of_range_symbol_result =
        Binary::from_raw(out_of_range_symbol.data(), out_of_range_symbol.size());
    if (out_of_range_symbol_result.is_err() &&
        out_of_range_symbol_result.unwrap_err() == eftl::elf::ParseError::InvalidRelocation) {
        return 0;
    }
    return 3;
}

auto test_elf32_observes_rel_record() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Rel32 = Traits32::Rel;
    using Section32 = Traits32::Section;
    using Symbol32 = Traits32::Symbol;

    constexpr usize K_SECTION_OFFSET = sizeof(Header32);
    constexpr usize K_STRING_OFFSET = 0x100;
    constexpr usize K_SYMBOL_OFFSET = 0x120;
    constexpr usize K_REL_OFFSET = 0x150;
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
    strings.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_OFFSET;
    strings.sh_size = 5;
    strings.sh_addralign = 1;
    Section32 symbols{};
    symbols.sh_type = static_cast<u32>(eftl::elf::SectionType::Symtab);
    symbols.sh_offset = K_SYMBOL_OFFSET;
    symbols.sh_size = 2 * sizeof(Symbol32);
    symbols.sh_link = 1;
    symbols.sh_info = 1;
    symbols.sh_addralign = alignof(Symbol32);
    symbols.sh_entsize = sizeof(Symbol32);
    Section32 rel{};
    rel.sh_type = static_cast<u32>(eftl::elf::SectionType::Rel);
    rel.sh_offset = K_REL_OFFSET;
    rel.sh_size = sizeof(Rel32);
    rel.sh_link = 2;
    rel.sh_addralign = alignof(Rel32);
    rel.sh_entsize = sizeof(Rel32);
    put(image, K_SECTION_OFFSET + sizeof(Section32), strings);
    put(image, K_SECTION_OFFSET + 2 * sizeof(Section32), symbols);
    put(image, K_SECTION_OFFSET + 3 * sizeof(Section32), rel);

    std::array<u8, 5> const names{0, 'f', 'o', 'o', 0};
    std::memcpy(image.data() + K_STRING_OFFSET, names.data(), names.size());
    Symbol32 symbol{};
    symbol.st_name = 1;
    put(image, K_SYMBOL_OFFSET + sizeof(Symbol32), symbol);
    Rel32 relocation{};
    relocation.r_offset = 0x1000;
    relocation.r_info = (1U << 8U) | 7U;
    put(image, K_REL_OFFSET, relocation);

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    ustl::Option<eftl::elf::RelocationTableView<Traits32, Rel32>> table = ustl::NONE;
    if (parsed.is_ok()) {
        table = parsed->rel_table(3);
    }
    ustl::Option<Rel32> record = ustl::NONE;
    if (table.is_some()) {
        record = table->relocation_at(0);
    }
    if (record.is_some() && record->r_offset == 0x1000 &&
        eftl::elf::RelocationTableView<Traits32, Rel32>::type(record.value()) == 7 &&
        eftl::elf::RelocationTableView<Traits32, Rel32>::symbol_index(record.value()) == 1) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_rel_and_rela_records(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_invalid_relocation_section_relationships(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_observes_big_endian_rel_and_rela_records(); result != 0) {
        return 20 + result;
    }
    if (test_elf32_observes_rel_record() == 0) {
        return 0;
    }
    return 30;
}
