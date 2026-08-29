/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>

#include <array>
#include <type_traits>

#include <cstddef>
#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using File = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Section = Traits::Section;
using Symbol = Traits::Symbol;

constexpr usize SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize STRING_TABLE_OFFSET = 0x200;
constexpr usize SYMBOL_TABLE_OFFSET = 0x220;
constexpr usize GROUP_OFFSET = 0x260;
constexpr usize SECOND_GROUP_OFFSET = 0x270;
constexpr usize MEMBER_OFFSET = 0x280;

template <typename Value, usize Size>
auto put_scalar(std::array<u8, Size> &image, usize offset, Value value, bool big_endian) -> void {
    static_assert(std::is_unsigned_v<Value>);
    if (!big_endian) {
        std::memcpy(image.data() + offset, &value, sizeof(value));
        return;
    }
    for (usize index = 0; index < sizeof(Value); ++index) {
        auto const shift = (sizeof(Value) - index - 1) * 8U;
        image[offset + index] = static_cast<u8>(value >> shift);
    }
}

template <usize Size>
auto put_header(std::array<u8, Size> &image, Header const &header, bool big_endian) -> void {
    std::memcpy(image.data(), header.e_ident, sizeof(header.e_ident));
    put_scalar(image, offsetof(Header, e_type), header.e_type, big_endian);
    put_scalar(image, offsetof(Header, e_machine), header.e_machine, big_endian);
    put_scalar(image, offsetof(Header, e_version), header.e_version, big_endian);
    put_scalar(image, offsetof(Header, e_entry), header.e_entry, big_endian);
    put_scalar(image, offsetof(Header, e_phoff), header.e_phoff, big_endian);
    put_scalar(image, offsetof(Header, e_shoff), header.e_shoff, big_endian);
    put_scalar(image, offsetof(Header, e_flags), header.e_flags, big_endian);
    put_scalar(image, offsetof(Header, e_ehsize), header.e_ehsize, big_endian);
    put_scalar(image, offsetof(Header, e_phentsize), header.e_phentsize, big_endian);
    put_scalar(image, offsetof(Header, e_phnum), header.e_phnum, big_endian);
    put_scalar(image, offsetof(Header, e_shentsize), header.e_shentsize, big_endian);
    put_scalar(image, offsetof(Header, e_shnum), header.e_shnum, big_endian);
    put_scalar(image, offsetof(Header, e_shstrndx), header.e_shstrndx, big_endian);
}

template <usize Size>
auto put_section(std::array<u8, Size> &image, usize offset, Section const &section, bool big_endian)
    -> void {
    put_scalar(image, offset + offsetof(Section, sh_name), section.sh_name, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_type), section.sh_type, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_flags), section.sh_flags, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_addr), section.sh_addr, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_offset), section.sh_offset, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_size), section.sh_size, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_link), section.sh_link, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_info), section.sh_info, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_addralign), section.sh_addralign, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_entsize), section.sh_entsize, big_endian);
}

template <usize Size>
auto put_symbol(std::array<u8, Size> &image, usize offset, Symbol const &symbol, bool big_endian)
    -> void {
    put_scalar(image, offset + offsetof(Symbol, st_name), symbol.st_name, big_endian);
    image[offset + offsetof(Symbol, st_info)] = symbol.st_info;
    image[offset + offsetof(Symbol, st_other)] = symbol.st_other;
    put_scalar(image, offset + offsetof(Symbol, st_shndx), symbol.st_shndx, big_endian);
    put_scalar(image, offset + offsetof(Symbol, st_value), symbol.st_value, big_endian);
    put_scalar(image, offset + offsetof(Symbol, st_size), symbol.st_size, big_endian);
}

auto make_header(bool big_endian) -> Header {
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits::ELF_CLASS;
    if (big_endian) {
        header.e_ident[5] = 2;
    } else {
        header.e_ident[5] = 1;
    }
    header.e_ident[6] = 1;
    header.e_type = 1;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_shoff = SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 5;
    return header;
}

auto make_group_image(bool big_endian = false) -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    put_header(image, make_header(big_endian), big_endian);

    Section strings{};
    strings.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = STRING_TABLE_OFFSET;
    strings.sh_size = 5;
    strings.sh_addralign = 1;
    put_section(image, SECTION_TABLE_OFFSET + sizeof(Section), strings, big_endian);

    Section symbols{};
    symbols.sh_type = static_cast<u32>(eftl::elf::SectionType::Symtab);
    symbols.sh_offset = SYMBOL_TABLE_OFFSET;
    symbols.sh_size = 2 * sizeof(Symbol);
    symbols.sh_link = 1;
    symbols.sh_info = 1;
    symbols.sh_addralign = alignof(Symbol);
    symbols.sh_entsize = sizeof(Symbol);
    put_section(image, SECTION_TABLE_OFFSET + 2 * sizeof(Section), symbols, big_endian);

    Section group{};
    group.sh_type = static_cast<u32>(eftl::elf::SectionType::Group);
    group.sh_offset = GROUP_OFFSET;
    group.sh_size = 2 * sizeof(u32);
    group.sh_link = 2;
    group.sh_info = 1;
    group.sh_addralign = alignof(u32);
    group.sh_entsize = sizeof(u32);
    put_section(image, SECTION_TABLE_OFFSET + 3 * sizeof(Section), group, big_endian);

    Section member{};
    member.sh_type = static_cast<u32>(eftl::elf::SectionType::Progbits);
    member.sh_flags = static_cast<u64>(eftl::elf::SectionFlags::Group);
    member.sh_offset = MEMBER_OFFSET;
    member.sh_size = 1;
    member.sh_addralign = 1;
    put_section(image, SECTION_TABLE_OFFSET + 4 * sizeof(Section), member, big_endian);

    std::array<u8, 5> const strings_data{0, 's', 'i', 'g', 0};
    std::memcpy(image.data() + STRING_TABLE_OFFSET, strings_data.data(), strings_data.size());
    Symbol signature{};
    signature.st_name = 1;
    put_symbol(image, SYMBOL_TABLE_OFFSET + sizeof(Symbol), signature, big_endian);
    put_scalar(image, GROUP_OFFSET, static_cast<u32>(1), big_endian);
    put_scalar(image, GROUP_OFFSET + sizeof(u32), static_cast<u32>(4), big_endian);
    image[MEMBER_OFFSET] = 0xcc;
    return image;
}

auto test_observes_comdat_group(bool big_endian = false) -> int {
    auto const bytes = make_group_image(big_endian);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    auto const group = [&]() -> ustl::Option<eftl::elf::SectionGroupView> {
        if (parsed.is_ok()) {
            return parsed->section_group(3);
        }
        return ustl::NONE;
    }();
    auto const member = [&]() -> ustl::Option<usize> {
        if (group.is_some()) {
            return group->member_section_at(0);
        }
        return ustl::NONE;
    }();
    if (group.is_some() && group->flags() == 1 && group->is_comdat() &&
        group->signature_symbol_index() == 1 && group->member_count() == 1 && member.is_some() &&
        member.value() == 4 && group->member_section_at(1).is_none()) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_group_relationships() -> int {
    auto invalid_signature = make_group_image();
    put_scalar(invalid_signature,
               SECTION_TABLE_OFFSET + 3 * sizeof(Section) + offsetof(Section, sh_link),
               static_cast<u32>(1), false);
    if (File::from_raw(invalid_signature.data(), invalid_signature.size()).is_ok()) {
        return 1;
    }

    auto unflagged_member = make_group_image();
    put_scalar(unflagged_member,
               SECTION_TABLE_OFFSET + 4 * sizeof(Section) + offsetof(Section, sh_flags),
               static_cast<u64>(0), false);
    if (File::from_raw(unflagged_member.data(), unflagged_member.size()).is_ok()) {
        return 2;
    }

    auto self_member = make_group_image();
    put_scalar(self_member, GROUP_OFFSET + sizeof(u32), static_cast<u32>(3), false);
    if (File::from_raw(self_member.data(), self_member.size()).is_ok()) {
        return 3;
    }

    auto duplicate_member = make_group_image();
    auto header = make_header(false);
    header.e_shnum = 6;
    put_header(duplicate_member, header, false);
    Section second_group{};
    second_group.sh_type = static_cast<u32>(eftl::elf::SectionType::Group);
    second_group.sh_offset = SECOND_GROUP_OFFSET;
    second_group.sh_size = 2 * sizeof(u32);
    second_group.sh_link = 2;
    second_group.sh_info = 1;
    second_group.sh_addralign = alignof(u32);
    second_group.sh_entsize = sizeof(u32);
    put_section(duplicate_member, SECTION_TABLE_OFFSET + 5 * sizeof(Section), second_group, false);
    put_scalar(duplicate_member, SECOND_GROUP_OFFSET, static_cast<u32>(0), false);
    put_scalar(duplicate_member, SECOND_GROUP_OFFSET + sizeof(u32), static_cast<u32>(4), false);
    if (File::from_raw(duplicate_member.data(), duplicate_member.size()).is_ok()) {
        return 4;
    }

    auto non_relocatable = make_group_image();
    put_scalar(non_relocatable, offsetof(Header, e_type), static_cast<u16>(2), false);
    if (File::from_raw(non_relocatable.data(), non_relocatable.size()).is_ok()) {
        return 5;
    }

    auto orphan_member = make_group_image();
    put_scalar(orphan_member,
               SECTION_TABLE_OFFSET + 3 * sizeof(Section) + offsetof(Section, sh_size),
               static_cast<u64>(sizeof(u32)), false);
    if (File::from_raw(orphan_member.data(), orphan_member.size()).is_err()) {
        return 0;
    }
    return 6;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_comdat_group(); result != 0) {
        return result;
    }
    if (auto const result = test_observes_comdat_group(true); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_rejects_invalid_group_relationships(); result != 0) {
        return 20 + result;
    }
    return 0;
}
