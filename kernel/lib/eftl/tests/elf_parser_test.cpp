/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/parser.hpp>
#include <eftl/elf/views.hpp>

#include <array>

#include <cstddef>
#include <cstring>

#include <ustl/variant.hpp>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Segment = Traits::Segment;
using Section = Traits::Section;

constexpr u16 ET_REL = 1;
constexpr u16 ET_EXEC = 2;
constexpr u16 ET_CORE = 4;

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

auto make_header(u16 type) -> Header {
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = type;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    return header;
}

auto make_empty_image(u16 type) -> std::array<u8, 512> {
    std::array<u8, 512> image{};
    auto const header = make_header(type);
    put(image, 0, header);
    return image;
}

template <usize Size>
auto put_big_endian_u16(std::array<u8, Size> &image, usize offset, u16 value) -> void {
    image[offset] = static_cast<u8>(value >> 8);
    image[offset + 1] = static_cast<u8>(value);
}

template <usize Size>
auto put_big_endian_u32(std::array<u8, Size> &image, usize offset, u32 value) -> void {
    image[offset] = static_cast<u8>(value >> 24);
    image[offset + 1] = static_cast<u8>(value >> 16);
    image[offset + 2] = static_cast<u8>(value >> 8);
    image[offset + 3] = static_cast<u8>(value);
}

template <usize Size>
auto put_big_endian_u64(std::array<u8, Size> &image, usize offset, u64 value) -> void {
    image[offset] = static_cast<u8>(value >> 56);
    image[offset + 1] = static_cast<u8>(value >> 48);
    image[offset + 2] = static_cast<u8>(value >> 40);
    image[offset + 3] = static_cast<u8>(value >> 32);
    image[offset + 4] = static_cast<u8>(value >> 24);
    image[offset + 5] = static_cast<u8>(value >> 16);
    image[offset + 6] = static_cast<u8>(value >> 8);
    image[offset + 7] = static_cast<u8>(value);
}

auto make_big_endian_empty_image(u16 type) -> std::array<u8, 512> {
    std::array<u8, 512> image{};
    std::memcpy(image.data(), eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    image[4] = Traits::ELF_CLASS;
    image[5] = 2;
    image[6] = 1;
    put_big_endian_u16(image, offsetof(Header, e_type), type);
    put_big_endian_u32(image, offsetof(Header, e_version), 1);
    put_big_endian_u16(image, offsetof(Header, e_ehsize), sizeof(Header));
    return image;
}

auto make_big_endian_image_with_load() -> std::array<u8, 512> {
    std::array<u8, 512> image{};
    std::memcpy(image.data(), eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    image[4] = Traits::ELF_CLASS;
    image[5] = 2;
    image[6] = 1;

    put_big_endian_u16(image, offsetof(Header, e_type), ET_EXEC);
    put_big_endian_u32(image, offsetof(Header, e_version), 1);
    put_big_endian_u64(image, offsetof(Header, e_entry), 0x400184);
    put_big_endian_u64(image, offsetof(Header, e_phoff), sizeof(Header));
    put_big_endian_u16(image, offsetof(Header, e_ehsize), sizeof(Header));
    put_big_endian_u16(image, offsetof(Header, e_phentsize), sizeof(Segment));
    put_big_endian_u16(image, offsetof(Header, e_phnum), 1);

    constexpr usize SEGMENT_OFFSET = sizeof(Header);
    put_big_endian_u32(image, SEGMENT_OFFSET + offsetof(Segment, p_type),
                       static_cast<u32>(eftl::elf::SegType::Load));
    put_big_endian_u32(
        image, SEGMENT_OFFSET + offsetof(Segment, p_flags),
        static_cast<u32>(eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable));
    put_big_endian_u64(image, SEGMENT_OFFSET + offsetof(Segment, p_offset), 0x180);
    put_big_endian_u64(image, SEGMENT_OFFSET + offsetof(Segment, p_vaddr), 0x400180);
    put_big_endian_u64(image, SEGMENT_OFFSET + offsetof(Segment, p_filesz), 8);
    put_big_endian_u64(image, SEGMENT_OFFSET + offsetof(Segment, p_memsz), 16);
    put_big_endian_u64(image, SEGMENT_OFFSET + offsetof(Segment, p_align), 8);
    return image;
}

auto make_big_endian_image_with_section() -> std::array<u8, 512> {
    std::array<u8, 512> image{};
    std::memcpy(image.data(), eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    image[4] = Traits::ELF_CLASS;
    image[5] = 2;
    image[6] = 1;

    put_big_endian_u16(image, offsetof(Header, e_type), ET_REL);
    put_big_endian_u32(image, offsetof(Header, e_version), 1);
    put_big_endian_u64(image, offsetof(Header, e_shoff), sizeof(Header));
    put_big_endian_u16(image, offsetof(Header, e_ehsize), sizeof(Header));
    put_big_endian_u16(image, offsetof(Header, e_shentsize), sizeof(Section));
    put_big_endian_u16(image, offsetof(Header, e_shnum), 2);

    constexpr usize DATA_OFFSET = 0x180;
    constexpr usize SECTION_OFFSET = sizeof(Header) + sizeof(Section);
    put_big_endian_u32(image, SECTION_OFFSET + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Strtab));
    put_big_endian_u64(image, SECTION_OFFSET + offsetof(Section, sh_offset), DATA_OFFSET);
    put_big_endian_u64(image, SECTION_OFFSET + offsetof(Section, sh_size), 1);
    put_big_endian_u64(image, SECTION_OFFSET + offsetof(Section, sh_addralign), 1);
    return image;
}

auto make_big_endian_image_with_note_section() -> std::array<u8, 512> {
    auto image = make_big_endian_image_with_section();
    constexpr usize NOTE_OFFSET = 0x180;
    constexpr usize NOTE_SIZE = 24;
    constexpr usize SECTION_OFFSET = sizeof(Header) + sizeof(Section);
    put_big_endian_u32(image, SECTION_OFFSET + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Note));
    put_big_endian_u64(image, SECTION_OFFSET + offsetof(Section, sh_size), NOTE_SIZE);
    put_big_endian_u32(image, NOTE_OFFSET, 0);
    put_big_endian_u32(image, NOTE_OFFSET + 4, 1);
    put_big_endian_u32(image, NOTE_OFFSET + 8, 7);
    image[NOTE_OFFSET + 16] = 0xab;
    return image;
}

auto make_big_endian_image_with_dynamic_section() -> std::array<u8, 512> {
    auto image = make_big_endian_image_with_section();
    constexpr usize STRING_TABLE_OFFSET = 0x180;
    constexpr usize DYNAMIC_TABLE_OFFSET = 0x1a0;
    constexpr usize STRING_SECTION_OFFSET = sizeof(Header) + sizeof(Section);
    constexpr usize DYNAMIC_SECTION_OFFSET = sizeof(Header) + 2 * sizeof(Section);
    put_big_endian_u16(image, offsetof(Header, e_shnum), 3);
    put_big_endian_u64(image, STRING_SECTION_OFFSET + offsetof(Section, sh_size), 5);
    std::array<u8, 5> const strings{0, 'f', 'o', 'o', 0};
    std::memcpy(image.data() + STRING_TABLE_OFFSET, strings.data(), strings.size());

    put_big_endian_u32(image, DYNAMIC_SECTION_OFFSET + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Dynamic));
    put_big_endian_u64(image, DYNAMIC_SECTION_OFFSET + offsetof(Section, sh_offset),
                       DYNAMIC_TABLE_OFFSET);
    put_big_endian_u64(image, DYNAMIC_SECTION_OFFSET + offsetof(Section, sh_size),
                       2 * sizeof(Traits::Dyn));
    put_big_endian_u32(image, DYNAMIC_SECTION_OFFSET + offsetof(Section, sh_link), 1);
    put_big_endian_u64(image, DYNAMIC_SECTION_OFFSET + offsetof(Section, sh_addralign), 8);
    put_big_endian_u64(image, DYNAMIC_SECTION_OFFSET + offsetof(Section, sh_entsize),
                       sizeof(Traits::Dyn));
    put_big_endian_u64(image, DYNAMIC_TABLE_OFFSET, static_cast<u64>(eftl::elf::DynTag::Needed));
    put_big_endian_u64(image, DYNAMIC_TABLE_OFFSET + sizeof(u64), 1);
    return image;
}

auto make_big_endian_image_with_symbol_section() -> std::array<u8, 512> {
    auto image = make_big_endian_image_with_section();
    constexpr usize STRING_TABLE_OFFSET = 0x160;
    constexpr usize SYMBOL_TABLE_OFFSET = 0x180;
    constexpr usize SECTION_INDEX_OFFSET = 0x1c0;
    constexpr usize STRING_SECTION_OFFSET = sizeof(Header) + sizeof(Section);
    constexpr usize SYMBOL_SECTION_OFFSET = sizeof(Header) + 2 * sizeof(Section);
    constexpr usize SECTION_INDEX_SECTION_OFFSET = sizeof(Header) + 3 * sizeof(Section);
    put_big_endian_u16(image, offsetof(Header, e_shnum), 4);
    put_big_endian_u64(image, STRING_SECTION_OFFSET + offsetof(Section, sh_offset),
                       STRING_TABLE_OFFSET);
    put_big_endian_u64(image, STRING_SECTION_OFFSET + offsetof(Section, sh_size), 5);
    std::array<u8, 5> const strings{0, 'f', 'o', 'o', 0};
    std::memcpy(image.data() + STRING_TABLE_OFFSET, strings.data(), strings.size());

    put_big_endian_u32(image, SYMBOL_SECTION_OFFSET + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Dynsym));
    put_big_endian_u64(image, SYMBOL_SECTION_OFFSET + offsetof(Section, sh_offset),
                       SYMBOL_TABLE_OFFSET);
    put_big_endian_u64(image, SYMBOL_SECTION_OFFSET + offsetof(Section, sh_size),
                       2 * sizeof(Traits::Symbol));
    put_big_endian_u32(image, SYMBOL_SECTION_OFFSET + offsetof(Section, sh_link), 1);
    put_big_endian_u32(image, SYMBOL_SECTION_OFFSET + offsetof(Section, sh_info), 1);
    put_big_endian_u64(image, SYMBOL_SECTION_OFFSET + offsetof(Section, sh_addralign), 8);
    put_big_endian_u64(image, SYMBOL_SECTION_OFFSET + offsetof(Section, sh_entsize),
                       sizeof(Traits::Symbol));

    put_big_endian_u32(image, SECTION_INDEX_SECTION_OFFSET + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::SymtabShndx));
    put_big_endian_u64(image, SECTION_INDEX_SECTION_OFFSET + offsetof(Section, sh_offset),
                       SECTION_INDEX_OFFSET);
    put_big_endian_u64(image, SECTION_INDEX_SECTION_OFFSET + offsetof(Section, sh_size), 8);
    put_big_endian_u32(image, SECTION_INDEX_SECTION_OFFSET + offsetof(Section, sh_link), 2);
    put_big_endian_u64(image, SECTION_INDEX_SECTION_OFFSET + offsetof(Section, sh_addralign), 4);
    put_big_endian_u64(image, SECTION_INDEX_SECTION_OFFSET + offsetof(Section, sh_entsize), 4);

    constexpr usize SYMBOL_OFFSET = SYMBOL_TABLE_OFFSET + sizeof(Traits::Symbol);
    put_big_endian_u32(image, SYMBOL_OFFSET, 1);
    image[SYMBOL_OFFSET + 4] = 0x12;
    image[SYMBOL_OFFSET + 5] = 3;
    put_big_endian_u16(image, SYMBOL_OFFSET + 6, eftl::elf::SHN_XINDEX);
    put_big_endian_u64(image, SYMBOL_OFFSET + 8, 0x400080);
    put_big_endian_u64(image, SYMBOL_OFFSET + 16, 0x10);
    put_big_endian_u32(image, SECTION_INDEX_OFFSET + 4, 3);
    return image;
}

auto make_image_with_segment(u16 type, Segment const &segment, u64 entrypoint = 0)
    -> std::array<u8, 512> {
    auto image = make_empty_image(type);
    auto header = make_header(type);
    header.e_entry = entrypoint;
    header.e_phoff = sizeof(Header);
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = 1;
    put(image, 0, header);
    put(image, header.e_phoff, segment);
    return image;
}

constexpr usize EXTENDED_PROGRAM_COUNT = static_cast<usize>(eftl::elf::PN_XNUM);
constexpr usize EXTENDED_SECTION_COUNT = static_cast<usize>(eftl::elf::SHN_LORESERVE) + 1;
constexpr usize PROGRAM_TABLE_OFFSET = 0x80;
constexpr usize SECTION_TABLE_OFFSET =
    PROGRAM_TABLE_OFFSET + EXTENDED_PROGRAM_COUNT * sizeof(Segment);
constexpr usize NAME_TABLE_OFFSET = SECTION_TABLE_OFFSET + EXTENDED_SECTION_COUNT * sizeof(Section);
constexpr usize TEXT_OFFSET = NAME_TABLE_OFFSET + 17;
using ExtendedImage = std::array<u8, TEXT_OFFSET + 4>;

auto make_extended_numbering_image() -> ExtendedImage & {
    static ExtendedImage image{};
    image.fill(0);

    auto header = make_header(ET_EXEC);
    header.e_phoff = PROGRAM_TABLE_OFFSET;
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = eftl::elf::PN_XNUM;
    header.e_shoff = SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 0;
    header.e_shstrndx = eftl::elf::SHN_XINDEX;
    put(image, 0, header);

    Section section_zero{};
    section_zero.sh_size = EXTENDED_SECTION_COUNT;
    section_zero.sh_link = eftl::elf::SHN_LORESERVE;
    section_zero.sh_info = EXTENDED_PROGRAM_COUNT;

    Section names{};
    names.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    names.sh_offset = NAME_TABLE_OFFSET;
    names.sh_size = 17;
    names.sh_addralign = 1;

    Section text{};
    text.sh_name = 11;
    text.sh_type = static_cast<u32>(eftl::elf::SectionType::Progbits);
    text.sh_offset = TEXT_OFFSET;
    text.sh_size = 4;
    text.sh_addralign = 4;

    put(image, SECTION_TABLE_OFFSET, section_zero);
    put(image,
        SECTION_TABLE_OFFSET + static_cast<usize>(eftl::elf::SHN_LORESERVE) * sizeof(Section),
        names);
    put(image, SECTION_TABLE_OFFSET + 2 * sizeof(Section), text);

    std::array<u8, 17> const string_table{
        0, '.', 's', 'h', 's', 't', 'r', 't', 'a', 'b', 0, '.', 't', 'e', 'x', 't', 0,
    };
    std::memcpy(image.data() + NAME_TABLE_OFFSET, string_table.data(), string_table.size());
    image[TEXT_OFFSET] = 0x12;
    image[TEXT_OFFSET + 1] = 0x34;
    image[TEXT_OFFSET + 2] = 0x56;
    image[TEXT_OFFSET + 3] = 0x78;
    return image;
}

auto test_rel_without_program_table() -> int {
    auto const image = make_empty_image(ET_REL);
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->elf_header().e_type != ET_REL) {
        return 2;
    }
    if (!parsed->segments().empty() || !parsed->sections().empty()) {
        return 3;
    }
    if (parsed->native_image().is_none()) {
        return 0;
    }
    return 4;
}

auto test_core_note_without_load() -> int {
    Segment note{};
    note.p_type = static_cast<u32>(eftl::elf::SegType::Note);
    auto const image = make_image_with_segment(ET_CORE, note);
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->segments().size() != 1) {
        return 2;
    }
    if (parsed->segments()[0].type() != eftl::elf::SegType::Note) {
        return 3;
    }
    if (parsed->native_image().is_none()) {
        return 0;
    }
    return 4;
}

auto test_rejects_truncated_program_table() -> int {
    auto image = make_empty_image(ET_EXEC);
    auto header = make_header(ET_EXEC);
    header.e_phoff = image.size() - sizeof(Segment) + 1;
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = 1;
    put(image, 0, header);

    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_ok()) {
        return 1;
    }
    if (parsed.unwrap_err() != eftl::elf::ParseError::InvalidSegment) {
        return 2;
    }
    return 0;
}

auto test_decodes_big_endian_header_and_segment() -> int {
    auto const image = make_big_endian_image_with_load();

    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->elf_header().e_type != ET_EXEC || parsed->elf_header().e_entry != 0x400184 ||
        parsed->segments().size() != 1) {
        return 2;
    }
    auto const segment = parsed->segments()[0];
    if (segment.type() != eftl::elf::SegType::Load || segment.p_offset != 0x180 ||
        segment.p_vaddr != 0x400180 || segment.p_filesz != 8 || segment.p_memsz != 16) {
        return 3;
    }
    auto const native_image = parsed->native_image();
    auto const file_offset = [&]() -> decltype(native_image->virtual_address_to_offset(0x400184)) {
        if (native_image.is_some()) {
            return native_image->virtual_address_to_offset(0x400184);
        }
        return ustl::NONE;
    }();
    if (file_offset.is_none() || file_offset.value() != 0x184) {
        return 4;
    }
    return 0;
}

auto test_decodes_big_endian_section() -> int {
    auto const image = make_big_endian_image_with_section();

    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->elf_header().e_shnum != 2 || parsed->sections().size() != 2) {
        return 2;
    }
    auto const section = parsed->sections()[1];
    if (static_cast<eftl::elf::SectionType>(section.sh_type) != eftl::elf::SectionType::Strtab ||
        section.sh_offset != 0x180 || section.sh_size != 1 || section.sh_addralign != 1) {
        return 3;
    }
    auto const data = parsed->section_data(1);
    if (data.is_none() || data->size() != 1 || data->data() != image.data() + 0x180) {
        return 4;
    }
    return 0;
}

auto test_observes_big_endian_note() -> int {
    auto const image = make_big_endian_image_with_note_section();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const notes = [&]() -> decltype(parsed->note_section(1)) {
        if (parsed.is_ok()) {
            return parsed->note_section(1);
        }
        return ustl::NONE;
    }();
    auto const note = [&]() -> decltype(notes->note_at(0)) {
        if (notes.is_some()) {
            return notes->note_at(0);
        }
        return ustl::NONE;
    }();
    if (note.is_some() && notes->size() == 1 && note->m_type == 7 && note->m_name.empty() &&
        note->m_descriptor.size() == 1 && note->m_descriptor.data()[0] == 0xab) {
        return 0;
    }
    return 1;
}

auto test_observes_big_endian_dynamic() -> int {
    auto const image = make_big_endian_image_with_dynamic_section();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const table = [&]() -> decltype(parsed->dynamic_table(2)) {
        if (parsed.is_ok()) {
            return parsed->dynamic_table(2);
        }
        return ustl::NONE;
    }();
    auto const entry = [&]() -> decltype(table->entry_at(0)) {
        if (table.is_some()) {
            return table->entry_at(0);
        }
        return ustl::NONE;
    }();
    auto const name = [&]() -> decltype(table->string_at(entry.value())) {
        if (entry.is_some() && table.is_some()) {
            return table->string_at(entry.value());
        }
        return ustl::NONE;
    }();
    if (table.is_some() && entry.is_some() && name.is_some() && table->size() == 1 &&
        eftl::elf::DynamicTableView<Traits>::tag(entry.value()) == eftl::elf::DynTag::Needed &&
        eftl::elf::DynamicTableView<Traits>::value(entry.value()) == 1 && name->size() == 3 &&
        name->data()[0] == 'f') {
        return 0;
    }
    return 1;
}

auto test_observes_big_endian_symbol_and_section_index() -> int {
    auto const image = make_big_endian_image_with_symbol_section();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const table = [&]() -> decltype(parsed->symbol_table(2)) {
        if (parsed.is_ok()) {
            return parsed->symbol_table(2);
        }
        return ustl::NONE;
    }();
    auto const symbol = [&]() -> decltype(table->symbol_at(1)) {
        if (table.is_some()) {
            return table->symbol_at(1);
        }
        return ustl::NONE;
    }();
    auto const name = [&]() -> decltype(table->symbol_name(1)) {
        if (table.is_some()) {
            return table->symbol_name(1);
        }
        return ustl::NONE;
    }();
    auto const section_index = [&]() -> decltype(table->section_index(1)) {
        if (table.is_some()) {
            return table->section_index(1);
        }
        return ustl::NONE;
    }();
    if (symbol.is_some() && name.is_some() && section_index.is_some() && table->size() == 2 &&
        symbol->st_value == 0x400080 && symbol->st_size == 0x10 &&
        eftl::elf::SymbolTableView<Traits>::binding(symbol.value()) == 1 &&
        eftl::elf::SymbolTableView<Traits>::type(symbol.value()) == 2 &&
        eftl::elf::SymbolTableView<Traits>::visibility(symbol.value()) == 3 && name->size() == 3 &&
        name->data()[0] == 'f' && section_index.value() == 3) {
        return 0;
    }
    return 1;
}

auto test_load_mapping_excludes_zero_fill() -> int {
    Segment load{};
    load.p_type = static_cast<u32>(eftl::elf::SegType::Load);
    load.p_flags =
        static_cast<u32>(eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    load.p_offset = 0x100;
    load.p_vaddr = 0x400100;
    load.p_filesz = 0x10;
    load.p_memsz = 0x20;
    load.p_align = 0x8;

    auto const image = make_image_with_segment(ET_EXEC, load, 0x400104);
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->elf_header().e_entry != 0x400104 || parsed->segments().size() != 1) {
        return 2;
    }
    if (parsed->segments()[0].p_vaddr != load.p_vaddr) {
        return 3;
    }
    auto const native_image = parsed->native_image();
    if (native_image.is_none() || native_image->entrypoint() != 0x400104) {
        return 4;
    }
    auto const file_offset = native_image->virtual_address_to_offset(0x400108);
    if (file_offset.is_none() || file_offset.value() != 0x108) {
        return 5;
    }
    if (native_image->virtual_address_to_offset(0x400110).is_some()) {
        return 6;
    }
    return 0;
}

auto test_load_mapping_preserves_separate_file_backed_intervals() -> int {
    auto image = make_empty_image(ET_EXEC);
    auto header = make_header(ET_EXEC);
    header.e_phoff = sizeof(Header);
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = 2;
    put(image, 0, header);

    Segment first{};
    first.p_type = static_cast<u32>(eftl::elf::SegType::Load);
    first.p_flags =
        static_cast<u32>(eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    first.p_offset = 0x100;
    first.p_vaddr = 0x400100;
    first.p_filesz = 0x10;
    first.p_memsz = 0x20;
    first.p_align = 0x100;

    Segment second{};
    second.p_type = static_cast<u32>(eftl::elf::SegType::Load);
    second.p_offset = 0x180;
    second.p_vaddr = 0x402000;
    second.p_filesz = 0x10;
    second.p_memsz = 0x10;
    second.p_align = 0x8;

    put(image, header.e_phoff, first);
    put(image, header.e_phoff + sizeof(Segment), second);

    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    auto const native_image = parsed->native_image();
    if (native_image.is_none()) {
        return 2;
    }
    auto const first_offset = native_image->virtual_address_to_offset(0x400108);
    auto const second_offset = native_image->virtual_address_to_offset(0x402008);
    if (first_offset.is_none() || first_offset.value() != 0x108 || second_offset.is_none() ||
        second_offset.value() != 0x188) {
        return 2;
    }
    if (native_image->virtual_address_to_offset(0x401000).is_some() ||
        native_image->virtual_address_to_offset(0x400110).is_some()) {
        return 3;
    }
    return 0;
}

auto test_elf64_resolves_extended_header_numbering() -> int {
    auto const &image = make_extended_numbering_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->segments().size() != EXTENDED_PROGRAM_COUNT ||
        parsed->sections().size() != EXTENDED_SECTION_COUNT || parsed->section_data(0).is_some()) {
        return 2;
    }
    auto const name = parsed->section_name(2);
    if (name.is_none() || name->size() != 5 || name->data()[0] != '.') {
        return 3;
    }
    return 0;
}

auto test_elf64_rejects_invalid_extended_header_numbering() -> int {
    auto missing_section_zero = make_empty_image(ET_EXEC);
    auto missing_header = make_header(ET_EXEC);
    missing_header.e_phnum = eftl::elf::PN_XNUM;
    put(missing_section_zero, 0, missing_header);
    auto const missing_section_zero_result =
        Binary::from_raw(missing_section_zero.data(), missing_section_zero.size());
    if (missing_section_zero_result.is_ok() ||
        missing_section_zero_result.unwrap_err() != eftl::elf::ParseError::InvalidSection) {
        return 1;
    }

    auto &zero_section_count = make_extended_numbering_image();
    Section section_zero{};
    std::memcpy(&section_zero, zero_section_count.data() + SECTION_TABLE_OFFSET,
                sizeof(section_zero));
    section_zero.sh_size = 0;
    put(zero_section_count, SECTION_TABLE_OFFSET, section_zero);
    auto const zero_section_count_result =
        Binary::from_raw(zero_section_count.data(), zero_section_count.size());
    if (zero_section_count_result.is_ok() ||
        zero_section_count_result.unwrap_err() != eftl::elf::ParseError::InvalidSection) {
        return 2;
    }

    auto &invalid_name_index = make_extended_numbering_image();
    std::memcpy(&section_zero, invalid_name_index.data() + SECTION_TABLE_OFFSET,
                sizeof(section_zero));
    section_zero.sh_link = static_cast<u32>(EXTENDED_SECTION_COUNT);
    put(invalid_name_index, SECTION_TABLE_OFFSET, section_zero);
    auto const invalid_name_index_result =
        Binary::from_raw(invalid_name_index.data(), invalid_name_index.size());
    if (invalid_name_index_result.is_err() &&
        invalid_name_index_result.unwrap_err() == eftl::elf::ParseError::InvalidSection) {
        return 0;
    }
    return 3;
}

auto test_elf64_rejects_noncanonical_extended_header_numbering() -> int {
    std::array<u8, 512> image{};
    auto header = make_header(ET_EXEC);
    header.e_phoff = sizeof(Header) + sizeof(Section);
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = eftl::elf::PN_XNUM;
    header.e_shoff = sizeof(Header);
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 1;
    put(image, 0, header);

    Section section_zero{};
    section_zero.sh_info = 1;
    put(image, header.e_shoff, section_zero);
    auto const small_program_count = Binary::from_raw(image.data(), image.size());
    if (small_program_count.is_ok() ||
        small_program_count.unwrap_err() != eftl::elf::ParseError::InvalidSegment) {
        return 1;
    }

    image.fill(0);
    header = make_header(ET_EXEC);
    header.e_shoff = sizeof(Header);
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 0;
    put(image, 0, header);

    section_zero = {};
    section_zero.sh_size = 1;
    put(image, header.e_shoff, section_zero);
    auto const small_section_count = Binary::from_raw(image.data(), image.size());
    if (small_section_count.is_ok() ||
        small_section_count.unwrap_err() != eftl::elf::ParseError::InvalidSection) {
        return 2;
    }

    image.fill(0);
    header.e_shnum = 2;
    header.e_shstrndx = eftl::elf::SHN_XINDEX;
    put(image, 0, header);
    section_zero = {};
    section_zero.sh_link = 1;
    put(image, header.e_shoff, section_zero);
    Section names{};
    names.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    names.sh_offset = sizeof(Header) + 2 * sizeof(Section);
    names.sh_size = 1;
    put(image, header.e_shoff + sizeof(Section), names);
    image[names.sh_offset] = 0;
    auto const small_name_index = Binary::from_raw(image.data(), image.size());
    if (small_name_index.is_err() &&
        small_name_index.unwrap_err() == eftl::elf::ParseError::InvalidSection) {
        return 0;
    }
    return 3;
}

auto test_elf64_rejects_reserved_ordinary_section_fields() -> int {
    auto &reserved_name_index = make_extended_numbering_image();
    Header header{};
    std::memcpy(&header, reserved_name_index.data(), sizeof(header));
    header.e_shstrndx = eftl::elf::SHN_LORESERVE;
    put(reserved_name_index, 0, header);
    auto const reserved_name_result =
        Binary::from_raw(reserved_name_index.data(), reserved_name_index.size());
    if (reserved_name_result.is_ok() ||
        reserved_name_result.unwrap_err() != eftl::elf::ParseError::InvalidSection) {
        return 1;
    }

    auto &reserved_section_count = make_extended_numbering_image();
    std::memcpy(&header, reserved_section_count.data(), sizeof(header));
    header.e_shnum = eftl::elf::SHN_LORESERVE;
    header.e_shstrndx = 0;
    put(reserved_section_count, 0, header);
    auto const reserved_count_result =
        Binary::from_raw(reserved_section_count.data(), reserved_section_count.size());
    if (reserved_count_result.is_err() &&
        reserved_count_result.unwrap_err() == eftl::elf::ParseError::InvalidSection) {
        return 0;
    }
    return 2;
}

auto test_elf32_rel_without_program_table() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Header32 = Traits32::Header;

    std::array<u8, sizeof(Header32)> image{};
    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits32::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = ET_REL;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header32);
    std::memcpy(image.data(), &header, sizeof(header));

    auto const parsed = eftl::elf::FileImpl<Traits32>::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->elf_header().e_type != ET_REL || !parsed->segments().empty()) {
        return 2;
    }
    return 0;
}

auto test_elf32_resolves_extended_program_count() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Segment32 = Traits32::Segment;
    using Section32 = Traits32::Section;

    constexpr usize ELF32_SECTION_TABLE_OFFSET =
        sizeof(Header32) + EXTENDED_PROGRAM_COUNT * sizeof(Segment32);
    static std::array<u8, ELF32_SECTION_TABLE_OFFSET + sizeof(Section32)> image{};
    image.fill(0);
    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits32::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = ET_REL;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header32);
    header.e_phoff = sizeof(Header32);
    header.e_phentsize = sizeof(Segment32);
    header.e_phnum = eftl::elf::PN_XNUM;
    header.e_shoff = ELF32_SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section32);
    header.e_shnum = 1;
    put(image, 0, header);

    Section32 section_zero{};
    section_zero.sh_info = EXTENDED_PROGRAM_COUNT;
    put(image, ELF32_SECTION_TABLE_OFFSET, section_zero);

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->segments().size() == EXTENDED_PROGRAM_COUNT && parsed->sections().size() == 1) {
        return 0;
    }
    return 1;
}

auto test_runtime_parser_dispatches_elf64() -> int {
    auto const image = make_empty_image(ET_EXEC);
    auto const parsed = eftl::elf::Parser::parse(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }

    auto const file = parsed.unwrap();
    auto const structural = file.type() == ET_EXEC && file.native_image().is_none();
    if (!structural) {
        return 2;
    }
    return 0;
}

auto test_runtime_parser_dispatches_elf32_without_program_table() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Header32 = Traits32::Header;

    std::array<u8, sizeof(Header32)> image{};
    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits32::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = ET_REL;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header32);
    std::memcpy(image.data(), &header, sizeof(header));

    auto const parsed = eftl::elf::Parser::parse(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }

    if (parsed.unwrap().class_type() != eftl::elf::Class::Elf32) {
        return 2;
    }
    return 0;
}

auto test_runtime_parser_rejects_invalid_identity() -> int {
    auto invalid_magic = make_empty_image(ET_EXEC);
    invalid_magic[0] = 0;
    auto const magic_result = eftl::elf::Parser::parse(invalid_magic.data(), invalid_magic.size());
    if (magic_result.is_ok() || magic_result.unwrap_err() != eftl::elf::ParseError::InvalidHeader) {
        return 1;
    }

    auto invalid_class = make_empty_image(ET_EXEC);
    invalid_class[4] = 3;
    auto const class_result = eftl::elf::Parser::parse(invalid_class.data(), invalid_class.size());
    if (class_result.is_ok() || class_result.unwrap_err() != eftl::elf::ParseError::InvalidHeader) {
        return 2;
    }

    auto invalid_byte_order = make_empty_image(ET_EXEC);
    invalid_byte_order[5] = 0;
    auto const byte_order_result =
        eftl::elf::Parser::parse(invalid_byte_order.data(), invalid_byte_order.size());
    if (byte_order_result.is_ok() ||
        byte_order_result.unwrap_err() != eftl::elf::ParseError::InvalidHeader) {
        return 3;
    }

    auto malformed_big_endian = make_big_endian_empty_image(ET_EXEC);
    malformed_big_endian[6] = 0;
    auto const malformed_big_endian_result =
        eftl::elf::Parser::parse(malformed_big_endian.data(), malformed_big_endian.size());
    if (malformed_big_endian_result.is_ok() ||
        malformed_big_endian_result.unwrap_err() != eftl::elf::ParseError::InvalidHeader) {
        return 4;
    }

    auto const empty_result = eftl::elf::Parser::parse(nullptr, 0);
    if (empty_result.is_ok() || empty_result.unwrap_err() != eftl::elf::ParseError::InvalidHeader) {
        return 5;
    }
    return 0;
}

auto test_runtime_parser_dispatches_big_endian() -> int {
    auto const image = make_big_endian_image_with_load();
    auto const parsed = eftl::elf::Parser::parse(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed.unwrap().entrypoint() != 0x400184) {
        return 2;
    }
    return 0;
}
} // namespace

auto main() -> int {
    if (auto const result = test_rel_without_program_table(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_core_note_without_load(); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_rejects_truncated_program_table(); result != 0) {
        return 30 + result;
    }
    if (auto const result = test_decodes_big_endian_header_and_segment(); result != 0) {
        return 40 + result;
    }
    if (auto const result = test_decodes_big_endian_section(); result != 0) {
        return 45 + result;
    }
    if (auto const result = test_observes_big_endian_note(); result != 0) {
        return 47 + result;
    }
    if (auto const result = test_observes_big_endian_dynamic(); result != 0) {
        return 49 + result;
    }
    if (auto const result = test_observes_big_endian_symbol_and_section_index(); result != 0) {
        return 51 + result;
    }
    if (auto const result = test_load_mapping_excludes_zero_fill(); result != 0) {
        return 50 + result;
    }
    if (auto const result = test_load_mapping_preserves_separate_file_backed_intervals();
        result != 0) {
        return 55 + result;
    }
    if (auto const result = test_elf64_resolves_extended_header_numbering(); result != 0) {
        return 60 + result;
    }
    if (auto const result = test_elf64_rejects_invalid_extended_header_numbering(); result != 0) {
        return 70 + result;
    }
    if (auto const result = test_elf64_rejects_noncanonical_extended_header_numbering();
        result != 0) {
        return 80 + result;
    }
    if (auto const result = test_elf64_rejects_reserved_ordinary_section_fields(); result != 0) {
        return 90 + result;
    }
    if (auto const result = test_elf32_rel_without_program_table(); result != 0) {
        return 100 + result;
    }
    if (auto const result = test_elf32_resolves_extended_program_count(); result != 0) {
        return 110 + result;
    }
    if (auto const result = test_runtime_parser_dispatches_elf64(); result != 0) {
        return 120 + result;
    }
    if (auto const result = test_runtime_parser_dispatches_elf32_without_program_table();
        result != 0) {
        return 130 + result;
    }
    if (auto const result = test_runtime_parser_rejects_invalid_identity(); result != 0) {
        return 140 + result;
    }
    if (auto const result = test_runtime_parser_dispatches_big_endian(); result != 0) {
        return 150 + result;
    }
    return 0;
}
