/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/note.hpp>

#include <array>

#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Segment = Traits::Segment;
using Section = Traits::Section;

constexpr usize SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize NOTE_OFFSET = 0x200;
constexpr usize NOTE_SIZE = 40;

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

template <usize Size>
auto put_bytes(std::array<u8, Size> &image, usize offset, u8 const *bytes, usize count) -> void {
    std::memcpy(image.data() + offset, bytes, count);
}

auto make_header() -> Header {
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 1;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_shoff = SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 2;
    return header;
}

auto make_note_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    auto const header = make_header();
    put(image, 0, header);

    Section notes{};
    notes.sh_type = static_cast<u32>(eftl::elf::SectionType::Note);
    notes.sh_offset = NOTE_OFFSET;
    notes.sh_size = NOTE_SIZE;
    notes.sh_addralign = 4;
    put(image, SECTION_TABLE_OFFSET + sizeof(Section), notes);

    eftl::elf::ElfNoteHeader first{.n_namesz = 5, .n_descsz = 3, .n_type = 7};
    put(image, NOTE_OFFSET, first);
    std::array<u8, 5> const name{'F', 'I', 'V', 'E', 0};
    std::array<u8, 3> const descriptor{1, 2, 3};
    put_bytes(image, NOTE_OFFSET + sizeof(first), name.data(), name.size());
    put_bytes(image, NOTE_OFFSET + 20, descriptor.data(), descriptor.size());

    eftl::elf::ElfNoteHeader second{.n_namesz = 0, .n_descsz = 4, .n_type = 9};
    put(image, NOTE_OFFSET + 24, second);
    std::array<u8, 4> const second_descriptor{4, 5, 6, 7};
    put_bytes(image, NOTE_OFFSET + 36, second_descriptor.data(), second_descriptor.size());
    return image;
}

auto test_observes_aligned_note_records() -> int {
    auto const image = make_note_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const notes = [&]() -> ustl::Option<eftl::elf::NoteSectionView<Traits>> {
        if (parsed.is_ok()) {
            return parsed->note_section(1);
        }
        return ustl::NONE;
    }();
    auto const first = [&]() -> ustl::Option<eftl::elf::NoteRecord> {
        if (notes.is_some()) {
            return notes->note_at(0);
        }
        return ustl::NONE;
    }();
    auto const second = [&]() -> ustl::Option<eftl::elf::NoteRecord> {
        if (notes.is_some()) {
            return notes->note_at(1);
        }
        return ustl::NONE;
    }();
    if (first.is_some() && second.is_some() && notes->size() == 2 && first->m_type == 7 &&
        first->m_name.size() == 5 && first->m_name.data()[0] == 'F' &&
        first->m_descriptor.size() == 3 && first->m_descriptor.data()[2] == 3 &&
        second->m_type == 9 && second->m_name.empty() && second->m_descriptor.size() == 4 &&
        second->m_descriptor.data()[0] == 4 && notes->note_at(2).is_none()) {
        return 0;
    }
    return 1;
}

auto test_rejects_malformed_note_envelopes() -> int {
    auto truncated_header = make_note_image();
    Section notes{};
    std::memcpy(&notes, truncated_header.data() + SECTION_TABLE_OFFSET + sizeof(Section),
                sizeof(notes));
    notes.sh_size = sizeof(eftl::elf::ElfNoteHeader) - 1;
    put(truncated_header, SECTION_TABLE_OFFSET + sizeof(Section), notes);
    auto const truncated_result =
        Binary::from_raw(truncated_header.data(), truncated_header.size());
    if (truncated_result.is_ok() ||
        truncated_result.unwrap_err() != eftl::elf::ParseError::InvalidNote) {
        return 1;
    }

    auto overrun_name = make_note_image();
    std::memcpy(&notes, overrun_name.data() + SECTION_TABLE_OFFSET + sizeof(Section),
                sizeof(notes));
    notes.sh_size = sizeof(eftl::elf::ElfNoteHeader);
    put(overrun_name, SECTION_TABLE_OFFSET + sizeof(Section), notes);
    auto const name_result = Binary::from_raw(overrun_name.data(), overrun_name.size());
    if (name_result.is_ok() || name_result.unwrap_err() != eftl::elf::ParseError::InvalidNote) {
        return 2;
    }

    auto missing_padding = make_note_image();
    eftl::elf::ElfNoteHeader unaligned{.n_namesz = 1, .n_descsz = 0, .n_type = 1};
    put(missing_padding, NOTE_OFFSET, unaligned);
    missing_padding[NOTE_OFFSET + sizeof(unaligned)] = 0;
    std::memcpy(&notes, missing_padding.data() + SECTION_TABLE_OFFSET + sizeof(Section),
                sizeof(notes));
    notes.sh_size = sizeof(unaligned) + 1;
    put(missing_padding, SECTION_TABLE_OFFSET + sizeof(Section), notes);
    auto const padding_result = Binary::from_raw(missing_padding.data(), missing_padding.size());
    if (padding_result.is_err() &&
        padding_result.unwrap_err() == eftl::elf::ParseError::InvalidNote) {
        return 0;
    }
    return 3;
}

auto test_elf32_observes_note_record() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Section32 = Traits32::Section;

    constexpr usize SECTION_OFFSET = sizeof(Header32);
    constexpr usize NOTE_SECTION_OFFSET = 0x100;
    std::array<u8, 512> image{};
    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits32::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 1;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header32);
    header.e_shoff = SECTION_OFFSET;
    header.e_shentsize = sizeof(Section32);
    header.e_shnum = 2;
    put(image, 0, header);

    Section32 notes{};
    notes.sh_type = static_cast<u32>(eftl::elf::SectionType::Note);
    notes.sh_offset = NOTE_SECTION_OFFSET;
    notes.sh_size = 20;
    notes.sh_addralign = 4;
    put(image, SECTION_OFFSET + sizeof(Section32), notes);

    eftl::elf::ElfNoteHeader record{.n_namesz = 1, .n_descsz = 1, .n_type = 3};
    put(image, NOTE_SECTION_OFFSET, record);
    image[NOTE_SECTION_OFFSET + sizeof(record)] = 0;
    image[NOTE_SECTION_OFFSET + 16] = 0xaa;

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    auto const note_section = [&]() -> ustl::Option<eftl::elf::NoteSectionView<Traits32>> {
        if (parsed.is_ok()) {
            return parsed->note_section(1);
        }
        return ustl::NONE;
    }();
    auto const note = [&]() -> ustl::Option<eftl::elf::NoteRecord> {
        if (note_section.is_some()) {
            return note_section->note_at(0);
        }
        return ustl::NONE;
    }();
    if (note.is_some() && note_section->size() == 1 && note->m_type == 3 &&
        note->m_name.size() == 1 && note->m_descriptor.size() == 1 &&
        note->m_descriptor.data()[0] == 0xaa) {
        return 0;
    }
    return 1;
}

auto test_core_file_observes_program_note() -> int {
    constexpr usize PROGRAM_TABLE_OFFSET = sizeof(Header);
    constexpr usize PROGRAM_NOTE_OFFSET = 0x200;
    std::array<u8, 1024> image{};
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 4;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_phoff = PROGRAM_TABLE_OFFSET;
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = 1;
    put(image, 0, header);

    Segment note_segment{};
    note_segment.p_type = static_cast<u32>(eftl::elf::SegType::Note);
    note_segment.p_offset = PROGRAM_NOTE_OFFSET;
    note_segment.p_filesz = 20;
    note_segment.p_memsz = 20;
    note_segment.p_align = alignof(u64);
    put(image, PROGRAM_TABLE_OFFSET, note_segment);

    eftl::elf::ElfNoteHeader record{.n_namesz = 4, .n_descsz = 4, .n_type = 1};
    put(image, PROGRAM_NOTE_OFFSET, record);
    std::array<u8, 4> const name{'C', 'O', 'R', 'E'};
    std::array<u8, 4> const descriptor{1, 2, 3, 4};
    put_bytes(image, PROGRAM_NOTE_OFFSET + sizeof(record), name.data(), name.size());
    put_bytes(image, PROGRAM_NOTE_OFFSET + 16, descriptor.data(), descriptor.size());

    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const notes = [&]() -> ustl::Option<eftl::elf::NoteSectionView<Traits>> {
        if (parsed.is_ok()) {
            return parsed->note_segment(0);
        }
        return ustl::NONE;
    }();
    auto const note = [&]() -> ustl::Option<eftl::elf::NoteRecord> {
        if (notes.is_some()) {
            return notes->note_at(0);
        }
        return ustl::NONE;
    }();
    if (note.is_some() && notes->size() == 1 && note->m_type == 1 && note->m_name.size() == 4 &&
        note->m_name.data()[0] == 'C' && note->m_descriptor.size() == 4 &&
        note->m_descriptor.data()[3] == 4) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_aligned_note_records(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_malformed_note_envelopes(); result != 0) {
        return 10 + result;
    }
    if (test_elf32_observes_note_record() != 0) {
        return 20;
    }
    if (test_core_file_observes_program_note() == 0) {
        return 0;
    }
    return 30;
}
