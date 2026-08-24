/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/section.hpp>

#include <array>

#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Section = Traits::Section;

constexpr usize K_SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize K_STRING_TABLE_OFFSET = 0x200;
constexpr usize K_TEXT_OFFSET = 0x220;

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
    header.e_shnum = 4;
    header.e_shstrndx = 1;
    return header;
}

auto make_sectioned_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    auto const header = make_header();
    put(image, 0, header);

    Section string_table{};
    string_table.sh_name = 1;
    string_table.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Strtab);
    string_table.sh_offset = K_STRING_TABLE_OFFSET;
    string_table.sh_size = 22;
    string_table.sh_addralign = 1;

    Section text{};
    text.sh_name = 11;
    text.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Progbits);
    text.sh_offset = K_TEXT_OFFSET;
    text.sh_size = 4;
    text.sh_addralign = 16;

    Section bss{};
    bss.sh_name = 17;
    bss.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Nobits);
    bss.sh_size = 0x20;
    bss.sh_addralign = 16;

    put(image, K_SECTION_TABLE_OFFSET + sizeof(Section), string_table);
    put(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), text);
    put(image, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), bss);

    std::array<u8, 22> const names{
        0,   '.', 's', 'h', 's', 't', 'r', 't', 'a', 'b', 0,
        '.', 't', 'e', 'x', 't', 0,   '.', 'b', 's', 's', 0,
    };
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());
    image[K_TEXT_OFFSET] = 0x12;
    image[K_TEXT_OFFSET + 1] = 0x34;
    image[K_TEXT_OFFSET + 2] = 0x56;
    image[K_TEXT_OFFSET + 3] = 0x78;
    return image;
}

auto test_string_table_returns_bounded_null_terminated_names() -> int {
    std::array<u8, 16> const bytes{
        0, '.', 't', 'e', 'x', 't', 0, '.', 's', 'h', 's', 't', 'r', 't', 'a', 'b',
    };
    auto const table =
        eftl::elf::StringTableView::from(eftl::ByteRange(bytes.data(), bytes.size()));
    if (table.is_none()) {
        return 1;
    }

    auto const text = table->string_at(1);
    if (text.is_none() || text->size() != 5 || text->data()[0] != '.') {
        return 2;
    }
    if (table->string_at(7).is_some() || table->string_at(bytes.size()).is_some()) {
        return 3;
    }
    return 0;
}

auto test_string_table_returns_the_empty_first_string() -> int {
    std::array<u8, 1> const bytes{0};
    auto const table =
        eftl::elf::StringTableView::from(eftl::ByteRange(bytes.data(), bytes.size()));
    auto const name = [&]() -> ustl::Option<eftl::ByteRange> {
        if (table.is_some()) {
            return table->string_at(0);
        }
        return ustl::NONE;
    }();
    if (name.is_some() && name->empty()) {
        return 0;
    }
    return 1;
}

auto test_binary_observes_validated_section_data_and_names() -> int {
    auto const image = make_sectioned_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }

    auto const text = parsed->section_data(2);
    auto const bss = parsed->section_data(3);
    auto const text_name = parsed->section_name(2);
    if (text.is_none() || text->size() != 4 || text->data()[0] != 0x12 || bss.is_some() ||
        text_name.is_none() || text_name->size() != 5 || text_name->data()[0] != '.') {
        return 2;
    }
    return 0;
}

auto test_binary_rejects_invalid_section_relationships() -> int {
    auto truncated_payload = make_sectioned_image();
    Section text{};
    std::memcpy(&text, truncated_payload.data() + K_SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(text));
    text.sh_offset = truncated_payload.size() - 1;
    text.sh_size = 2;
    put(truncated_payload, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), text);
    if (Binary::from_raw(truncated_payload.data(), truncated_payload.size()).is_ok()) {
        return 1;
    }

    auto invalid_name_index = make_sectioned_image();
    auto header = make_header();
    header.e_shstrndx = 4;
    put(invalid_name_index, 0, header);
    if (Binary::from_raw(invalid_name_index.data(), invalid_name_index.size()).is_ok()) {
        return 2;
    }

    auto invalid_name_table = make_sectioned_image();
    Section string_table{};
    std::memcpy(&string_table, invalid_name_table.data() + K_SECTION_TABLE_OFFSET + sizeof(Section),
                sizeof(string_table));
    string_table.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Progbits);
    put(invalid_name_table, K_SECTION_TABLE_OFFSET + sizeof(Section), string_table);
    if (Binary::from_raw(invalid_name_table.data(), invalid_name_table.size()).is_ok()) {
        return 3;
    }

    auto invalid_entry_size = make_sectioned_image();
    std::memcpy(&text, invalid_entry_size.data() + K_SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(text));
    text.sh_entsize = 3;
    put(invalid_entry_size, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), text);
    if (Binary::from_raw(invalid_entry_size.data(), invalid_entry_size.size()).is_err()) {
        return 0;
    }
    return 4;
}
} // namespace

auto main() -> int {
    if (auto const result = test_string_table_returns_bounded_null_terminated_names();
        result != 0) {
        return result;
    }
    if (auto const result = test_string_table_returns_the_empty_first_string(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_binary_observes_validated_section_data_and_names(); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_binary_rejects_invalid_section_relationships(); result != 0) {
        return 30 + result;
    }
    return 0;
}
