/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/version_symbol.hpp>

#include <array>

#include <cstddef>
#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Section = Traits::Section;
using Symbol = Traits::Symbol;
using Version = Traits::Half;

constexpr usize K_SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize K_STRING_TABLE_OFFSET = 0x200;
constexpr usize K_DYNSYM_OFFSET = 0x240;
constexpr usize K_VERSYM_OFFSET = 0x280;

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
    image[offset] = static_cast<u8>(value >> 24U);
    image[offset + 1] = static_cast<u8>(value >> 16U);
    image[offset + 2] = static_cast<u8>(value >> 8U);
    image[offset + 3] = static_cast<u8>(value);
}

template <usize Size>
auto put_big_endian_u64(std::array<u8, Size> &image, usize offset, u64 value)
    -> void {
    for (usize index = 0; index < sizeof(value); ++index) {
        image[offset + index] = static_cast<u8>(value >> ((7 - index) * 8U));
    }
}

auto make_version_symbol_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_shoff = K_SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 4;
    put(image, 0, header);

    Section strings{};
    strings.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_TABLE_OFFSET;
    strings.sh_size = 5;
    strings.sh_addralign = 1;

    Section dynamic_symbols{};
    dynamic_symbols.sh_type = static_cast<u32>(eftl::elf::SectionType::Dynsym);
    dynamic_symbols.sh_offset = K_DYNSYM_OFFSET;
    dynamic_symbols.sh_size = 2 * sizeof(Symbol);
    dynamic_symbols.sh_link = 1;
    dynamic_symbols.sh_info = 1;
    dynamic_symbols.sh_addralign = alignof(Symbol);
    dynamic_symbols.sh_entsize = sizeof(Symbol);

    Section versions{};
    versions.sh_type = static_cast<u32>(eftl::elf::SectionType::GnuVersym);
    versions.sh_offset = K_VERSYM_OFFSET;
    versions.sh_size = 2 * sizeof(Version);
    versions.sh_link = 2;
    versions.sh_addralign = alignof(Version);
    versions.sh_entsize = sizeof(Version);

    put(image, K_SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), dynamic_symbols);
    put(image, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), versions);

    std::array<u8, 5> const names{0, 'f', 'o', 'o', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());
    Symbol symbol{};
    symbol.st_name = 1;
    put(image, K_DYNSYM_OFFSET + sizeof(Symbol), symbol);
    std::array<Version, 2> const version_entries{1, 0x8002U};
    std::memcpy(image.data() + K_VERSYM_OFFSET, version_entries.data(), sizeof(version_entries));
    return image;
}

auto make_big_endian_version_symbol_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    std::memcpy(image.data(), eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    image[4] = Traits::K_ELF_CLASS;
    image[5] = 2;
    image[6] = 1;
    put_big_endian_u16(image, offsetof(Header, e_type), 3);
    put_big_endian_u32(image, offsetof(Header, e_version), 1);
    put_big_endian_u16(image, offsetof(Header, e_ehsize), sizeof(Header));
    put_big_endian_u64(image, offsetof(Header, e_shoff), K_SECTION_TABLE_OFFSET);
    put_big_endian_u16(image, offsetof(Header, e_shentsize), sizeof(Section));
    put_big_endian_u16(image, offsetof(Header, e_shnum), 4);

    constexpr usize K_STRING_SECTION = K_SECTION_TABLE_OFFSET + sizeof(Section);
    constexpr usize K_SYMBOL_SECTION = K_SECTION_TABLE_OFFSET + 2 * sizeof(Section);
    constexpr usize K_VERSION_SECTION = K_SECTION_TABLE_OFFSET + 3 * sizeof(Section);
    put_big_endian_u32(image, K_STRING_SECTION + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Strtab));
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_offset),
                       K_STRING_TABLE_OFFSET);
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_size), 5);
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_addralign), 1);

    put_big_endian_u32(image, K_SYMBOL_SECTION + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Dynsym));
    put_big_endian_u64(image, K_SYMBOL_SECTION + offsetof(Section, sh_offset), K_DYNSYM_OFFSET);
    put_big_endian_u64(image, K_SYMBOL_SECTION + offsetof(Section, sh_size), 2 * sizeof(Symbol));
    put_big_endian_u32(image, K_SYMBOL_SECTION + offsetof(Section, sh_link), 1);
    put_big_endian_u32(image, K_SYMBOL_SECTION + offsetof(Section, sh_info), 1);
    put_big_endian_u64(image, K_SYMBOL_SECTION + offsetof(Section, sh_addralign), alignof(Symbol));
    put_big_endian_u64(image, K_SYMBOL_SECTION + offsetof(Section, sh_entsize), sizeof(Symbol));

    put_big_endian_u32(image, K_VERSION_SECTION + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::GnuVersym));
    put_big_endian_u64(image, K_VERSION_SECTION + offsetof(Section, sh_offset), K_VERSYM_OFFSET);
    put_big_endian_u64(image, K_VERSION_SECTION + offsetof(Section, sh_size), 2 * sizeof(Version));
    put_big_endian_u32(image, K_VERSION_SECTION + offsetof(Section, sh_link), 2);
    put_big_endian_u64(image, K_VERSION_SECTION + offsetof(Section, sh_addralign),
                       alignof(Version));
    put_big_endian_u64(image, K_VERSION_SECTION + offsetof(Section, sh_entsize), sizeof(Version));

    std::array<u8, 5> const names{0, 'f', 'o', 'o', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());
    put_big_endian_u32(image, K_DYNSYM_OFFSET + sizeof(Symbol), 1);
    put_big_endian_u16(image, K_VERSYM_OFFSET, 1);
    put_big_endian_u16(image, K_VERSYM_OFFSET + sizeof(Version), 0x8002U);
    return image;
}

auto test_observes_version_symbols() -> int {
    auto const image = make_version_symbol_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const versions = [&]() -> ustl::Option<eftl::elf::VersionSymbolTableView<Traits>> {
        if (parsed.is_ok()) {
            return parsed->version_symbol_table(3);
        }
        return ustl::NONE;
    }();
    auto const global = [&]() -> ustl::Option<Version> {
        if (versions.is_some()) {
            return versions->version_at(0);
        }
        return ustl::NONE;
    }();
    auto const hidden = [&]() -> ustl::Option<Version> {
        if (versions.is_some()) {
            return versions->version_at(1);
        }
        return ustl::NONE;
    }();
    if (versions.is_some() && versions->size() == 2 && global.is_some() && hidden.is_some() &&
        eftl::elf::VersionSymbolTableView<Traits>::version_index(global.value()) == 1 &&
        !eftl::elf::VersionSymbolTableView<Traits>::is_hidden(global.value()) &&
        eftl::elf::VersionSymbolTableView<Traits>::version_index(hidden.value()) == 2 &&
        eftl::elf::VersionSymbolTableView<Traits>::is_hidden(hidden.value()) &&
        versions->version_at(2).is_none() && parsed->version_symbol_table(2).is_none()) {
        return 0;
    }
    return 1;
}

auto test_observes_big_endian_version_symbols() -> int {
    auto const image = make_big_endian_version_symbol_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const versions = [&]() -> ustl::Option<eftl::elf::VersionSymbolTableView<Traits>> {
        if (parsed.is_ok()) {
            return parsed->version_symbol_table(3);
        }
        return ustl::NONE;
    }();
    auto const global = [&]() -> ustl::Option<Version> {
        if (versions.is_some()) {
            return versions->version_at(0);
        }
        return ustl::NONE;
    }();
    auto const hidden = [&]() -> ustl::Option<Version> {
        if (versions.is_some()) {
            return versions->version_at(1);
        }
        return ustl::NONE;
    }();
    if (global.is_some() && hidden.is_some() &&
        eftl::elf::VersionSymbolTableView<Traits>::version_index(global.value()) == 1 &&
        !eftl::elf::VersionSymbolTableView<Traits>::is_hidden(global.value()) &&
        eftl::elf::VersionSymbolTableView<Traits>::version_index(hidden.value()) == 2 &&
        eftl::elf::VersionSymbolTableView<Traits>::is_hidden(hidden.value())) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_version_symbol_relationships() -> int {
    auto wrong_entry_size = make_version_symbol_image();
    Section versions{};
    std::memcpy(&versions, wrong_entry_size.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(versions));
    versions.sh_entsize = sizeof(u32);
    put(wrong_entry_size, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), versions);
    auto const wrong_entry_size_result =
        Binary::from_raw(wrong_entry_size.data(), wrong_entry_size.size());
    if (wrong_entry_size_result.is_ok() ||
        wrong_entry_size_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 1;
    }

    auto wrong_link = make_version_symbol_image();
    std::memcpy(&versions, wrong_link.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(versions));
    versions.sh_link = 1;
    put(wrong_link, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), versions);
    auto const wrong_link_result = Binary::from_raw(wrong_link.data(), wrong_link.size());
    if (wrong_link_result.is_ok() ||
        wrong_link_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 2;
    }

    auto mismatched_count = make_version_symbol_image();
    std::memcpy(&versions, mismatched_count.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(versions));
    versions.sh_size = sizeof(Version);
    put(mismatched_count, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), versions);
    auto const mismatched_count_result =
        Binary::from_raw(mismatched_count.data(), mismatched_count.size());
    if (mismatched_count_result.is_ok() ||
        mismatched_count_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 3;
    }

    auto duplicate_versions = make_version_symbol_image();
    Header header{};
    std::memcpy(&header, duplicate_versions.data(), sizeof(header));
    header.e_shnum = 5;
    put(duplicate_versions, 0, header);
    std::memcpy(&versions, duplicate_versions.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(versions));
    put(duplicate_versions, K_SECTION_TABLE_OFFSET + 4 * sizeof(Section), versions);
    auto const duplicate_versions_result =
        Binary::from_raw(duplicate_versions.data(), duplicate_versions.size());
    if (duplicate_versions_result.is_err() &&
        duplicate_versions_result.unwrap_err() == eftl::elf::ParseError::InvalidVersion) {
        return 0;
    }
    return 4;
}

auto test_elf32_observes_version_symbols() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Section32 = Traits32::Section;
    using Symbol32 = Traits32::Symbol;
    using Version32 = Traits32::Half;

    constexpr usize K_SECTION_OFFSET = sizeof(Header32);
    constexpr usize K_STRING_OFFSET = 0x100;
    constexpr usize K_DYNSYM_OFFSET_32 = 0x120;
    constexpr usize K_VERSYM_OFFSET_32 = 0x150;
    std::array<u8, 512> image{};

    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits32::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 3;
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
    Section32 dynamic_symbols{};
    dynamic_symbols.sh_type = static_cast<u32>(eftl::elf::SectionType::Dynsym);
    dynamic_symbols.sh_offset = K_DYNSYM_OFFSET_32;
    dynamic_symbols.sh_size = 2 * sizeof(Symbol32);
    dynamic_symbols.sh_link = 1;
    dynamic_symbols.sh_info = 1;
    dynamic_symbols.sh_addralign = alignof(Symbol32);
    dynamic_symbols.sh_entsize = sizeof(Symbol32);
    Section32 versions{};
    versions.sh_type = static_cast<u32>(eftl::elf::SectionType::GnuVersym);
    versions.sh_offset = K_VERSYM_OFFSET_32;
    versions.sh_size = 2 * sizeof(Version32);
    versions.sh_link = 2;
    versions.sh_addralign = alignof(Version32);
    versions.sh_entsize = sizeof(Version32);
    put(image, K_SECTION_OFFSET + sizeof(Section32), strings);
    put(image, K_SECTION_OFFSET + 2 * sizeof(Section32), dynamic_symbols);
    put(image, K_SECTION_OFFSET + 3 * sizeof(Section32), versions);

    std::array<u8, 5> const names{0, 'f', 'o', 'o', 0};
    std::memcpy(image.data() + K_STRING_OFFSET, names.data(), names.size());
    Symbol32 symbol{};
    symbol.st_name = 1;
    put(image, K_DYNSYM_OFFSET_32 + sizeof(Symbol32), symbol);
    std::array<Version32, 2> const version_entries{0, 0x8003U};
    std::memcpy(image.data() + K_VERSYM_OFFSET_32, version_entries.data(), sizeof(version_entries));

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    auto const versions_table = [&]() -> ustl::Option<eftl::elf::VersionSymbolTableView<Traits32>> {
        if (parsed.is_ok()) {
            return parsed->version_symbol_table(3);
        }
        return ustl::NONE;
    }();
    auto const hidden = [&]() -> ustl::Option<Version32> {
        if (versions_table.is_some()) {
            return versions_table->version_at(1);
        }
        return ustl::NONE;
    }();
    if (hidden.is_some() &&
        eftl::elf::VersionSymbolTableView<Traits32>::version_index(hidden.value()) == 3 &&
        eftl::elf::VersionSymbolTableView<Traits32>::is_hidden(hidden.value())) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_version_symbols(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_invalid_version_symbol_relationships(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_observes_big_endian_version_symbols(); result != 0) {
        return 20 + result;
    }
    if (test_elf32_observes_version_symbols() == 0) {
        return 0;
    }
    return 30;
}
