/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/dynamic_section.hpp>

#include <array>

#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Dynamic = Traits::Dyn;
using Header = Traits::Header;
using Section = Traits::Section;

constexpr usize SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize STRING_TABLE_OFFSET = 0x200;
constexpr usize DYNAMIC_TABLE_OFFSET = 0x240;

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

auto make_header() -> Header {
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_shoff = SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 3;
    return header;
}

auto make_dynamic_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    auto const header = make_header();
    put(image, 0, header);

    Section strings{};
    strings.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = STRING_TABLE_OFFSET;
    strings.sh_size = 24;
    strings.sh_addralign = 1;

    Section dynamics{};
    dynamics.sh_type = static_cast<u32>(eftl::elf::SectionType::Dynamic);
    dynamics.sh_offset = DYNAMIC_TABLE_OFFSET;
    dynamics.sh_size = 3 * sizeof(Dynamic);
    dynamics.sh_link = 1;
    dynamics.sh_addralign = alignof(Dynamic);
    dynamics.sh_entsize = sizeof(Dynamic);

    put(image, SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put(image, SECTION_TABLE_OFFSET + 2 * sizeof(Section), dynamics);

    std::array<u8, 24> const names{
        0, 'l', 'i', 'b', 'a', 'l', 'p', 'h', 'a', '.', 's', 'o',
        0, 'l', 'i', 'b', 's', 'e', 'l', 'f', '.', 's', 'o', 0,
    };
    std::memcpy(image.data() + STRING_TABLE_OFFSET, names.data(), names.size());

    Dynamic needed{};
    needed.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Needed);
    needed.d_val = 1;
    Dynamic soname{};
    soname.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::SoName);
    soname.d_val = 13;
    Dynamic terminator{};
    terminator.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::DtNull);
    put(image, DYNAMIC_TABLE_OFFSET, needed);
    put(image, DYNAMIC_TABLE_OFFSET + sizeof(Dynamic), soname);
    put(image, DYNAMIC_TABLE_OFFSET + 2 * sizeof(Dynamic), terminator);
    return image;
}

auto test_observes_dynamic_entries_and_names() -> int {
    auto const image = make_dynamic_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const table = [&]() -> decltype(parsed->dynamic_table(2)) {
        if (parsed.is_ok()) {
            return parsed->dynamic_table(2);
        }
        return ustl::NONE;
    }();
    auto const needed = [&]() -> decltype(table->entry_at(0)) {
        if (table.is_some()) {
            return table->entry_at(0);
        }
        return ustl::NONE;
    }();
    auto const soname = [&]() -> decltype(table->entry_at(1)) {
        if (table.is_some()) {
            return table->entry_at(1);
        }
        return ustl::NONE;
    }();
    auto const needed_name = [&]() -> decltype(table->string_at(needed.value())) {
        if (needed.is_some() && table.is_some()) {
            return table->string_at(needed.value());
        }
        return ustl::NONE;
    }();
    auto const soname_name = [&]() -> decltype(table->string_at(soname.value())) {
        if (soname.is_some() && table.is_some()) {
            return table->string_at(soname.value());
        }
        return ustl::NONE;
    }();
    auto const linkage = [&]() -> decltype(table->linkage()) {
        if (table.is_some()) {
            return table->linkage();
        }
        return ustl::NONE;
    }();
    auto const linked_needed = [&]() -> decltype(linkage->needed_library_at(0)) {
        if (linkage.is_some()) {
            return linkage->needed_library_at(0);
        }
        return ustl::NONE;
    }();
    auto const linked_soname = [&]() -> decltype(linkage->soname()) {
        if (linkage.is_some()) {
            return linkage->soname();
        }
        return ustl::NONE;
    }();
    if (table.is_some() && table->size() == 2 && needed.is_some() && soname.is_some() &&
        needed_name.is_some() && soname_name.is_some() &&
        eftl::elf::DynamicTableView<Traits>::tag(needed.value()) == eftl::elf::DynTag::Needed &&
        eftl::elf::DynamicTableView<Traits>::value(needed.value()) == 1 &&
        eftl::elf::DynamicTableView<Traits>::tag(soname.value()) == eftl::elf::DynTag::SoName &&
        eftl::elf::DynamicTableView<Traits>::value(soname.value()) == 13 &&
        needed_name->size() == 11 && needed_name->data()[0] == 'l' && soname_name->size() == 10 &&
        soname_name->data()[3] == 's' && linkage.is_some() &&
        linkage->needed_library_count() == 1 && linked_needed.is_some() &&
        linked_needed->size() == 11 && linked_needed->data()[3] == 'a' && linked_soname.is_some() &&
        linked_soname->size() == 10 && linked_soname->data()[3] == 's' &&
        linkage->rpath().is_none() && linkage->runpath().is_none() && linkage->flags().is_none() &&
        linkage->flags1().is_none() && table->entry_at(2).is_none() &&
        parsed->dynamic_table(1).is_none()) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_dynamic_section_relationships() -> int {
    auto wrong_entry_size = make_dynamic_image();
    Section dynamics{};
    std::memcpy(&dynamics, wrong_entry_size.data() + SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(dynamics));
    dynamics.sh_size = 2 * sizeof(Dynamic);
    dynamics.sh_entsize = 2 * sizeof(Dynamic);
    put(wrong_entry_size, SECTION_TABLE_OFFSET + 2 * sizeof(Section), dynamics);
    auto const wrong_entry_size_result =
        Binary::from_raw(wrong_entry_size.data(), wrong_entry_size.size());
    if (wrong_entry_size_result.is_ok() ||
        wrong_entry_size_result.unwrap_err() != eftl::elf::ParseError::InvalidDynamic) {
        return 1;
    }

    auto wrong_link = make_dynamic_image();
    std::memcpy(&dynamics, wrong_link.data() + SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(dynamics));
    dynamics.sh_link = 0;
    put(wrong_link, SECTION_TABLE_OFFSET + 2 * sizeof(Section), dynamics);
    auto const wrong_link_result = Binary::from_raw(wrong_link.data(), wrong_link.size());
    if (wrong_link_result.is_ok() ||
        wrong_link_result.unwrap_err() != eftl::elf::ParseError::InvalidDynamic) {
        return 2;
    }

    auto nonzero_info = make_dynamic_image();
    std::memcpy(&dynamics, nonzero_info.data() + SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(dynamics));
    dynamics.sh_info = 1;
    put(nonzero_info, SECTION_TABLE_OFFSET + 2 * sizeof(Section), dynamics);
    auto const nonzero_info_result = Binary::from_raw(nonzero_info.data(), nonzero_info.size());
    if (nonzero_info_result.is_ok() ||
        nonzero_info_result.unwrap_err() != eftl::elf::ParseError::InvalidDynamic) {
        return 3;
    }

    auto missing_terminator = make_dynamic_image();
    Dynamic entry{};
    std::memcpy(&entry, missing_terminator.data() + DYNAMIC_TABLE_OFFSET + 2 * sizeof(Dynamic),
                sizeof(entry));
    entry.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Needed);
    entry.d_val = 1;
    put(missing_terminator, DYNAMIC_TABLE_OFFSET + 2 * sizeof(Dynamic), entry);
    auto const missing_terminator_result =
        Binary::from_raw(missing_terminator.data(), missing_terminator.size());
    if (missing_terminator_result.is_ok() ||
        missing_terminator_result.unwrap_err() != eftl::elf::ParseError::InvalidDynamic) {
        return 4;
    }

    auto nonnull_after_terminator = make_dynamic_image();
    std::memcpy(&entry, nonnull_after_terminator.data() + DYNAMIC_TABLE_OFFSET + sizeof(Dynamic),
                sizeof(entry));
    entry.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::DtNull);
    put(nonnull_after_terminator, DYNAMIC_TABLE_OFFSET + sizeof(Dynamic), entry);
    entry.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Needed);
    entry.d_val = 1;
    put(nonnull_after_terminator, DYNAMIC_TABLE_OFFSET + 2 * sizeof(Dynamic), entry);
    auto const nonnull_after_terminator_result =
        Binary::from_raw(nonnull_after_terminator.data(), nonnull_after_terminator.size());
    if (nonnull_after_terminator_result.is_ok() ||
        nonnull_after_terminator_result.unwrap_err() != eftl::elf::ParseError::InvalidDynamic) {
        return 5;
    }

    auto invalid_needed_name = make_dynamic_image();
    std::memcpy(&entry, invalid_needed_name.data() + DYNAMIC_TABLE_OFFSET, sizeof(entry));
    entry.d_val = 24;
    put(invalid_needed_name, DYNAMIC_TABLE_OFFSET, entry);
    auto const invalid_needed_name_result =
        Binary::from_raw(invalid_needed_name.data(), invalid_needed_name.size());
    if (invalid_needed_name_result.is_err() &&
        invalid_needed_name_result.unwrap_err() == eftl::elf::ParseError::InvalidDynamic) {
        return 0;
    }
    return 6;
}

auto test_elf32_observes_dynamic_entries() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Dynamic32 = Traits32::Dyn;
    using Header32 = Traits32::Header;
    using Section32 = Traits32::Section;

    constexpr usize SECTION_OFFSET = sizeof(Header32);
    constexpr usize STRING_OFFSET = 0x100;
    constexpr usize DYNAMIC_OFFSET = 0x120;
    std::array<u8, 512> image{};

    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits32::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header32);
    header.e_shoff = SECTION_OFFSET;
    header.e_shentsize = sizeof(Section32);
    header.e_shnum = 3;
    put(image, 0, header);

    Section32 strings{};
    strings.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = STRING_OFFSET;
    strings.sh_size = 5;
    strings.sh_addralign = 1;
    Section32 dynamics{};
    dynamics.sh_type = static_cast<u32>(eftl::elf::SectionType::Dynamic);
    dynamics.sh_offset = DYNAMIC_OFFSET;
    dynamics.sh_size = 3 * sizeof(Dynamic32);
    dynamics.sh_link = 1;
    dynamics.sh_addralign = alignof(Dynamic32);
    dynamics.sh_entsize = sizeof(Dynamic32);
    put(image, SECTION_OFFSET + sizeof(Section32), strings);
    put(image, SECTION_OFFSET + 2 * sizeof(Section32), dynamics);

    std::array<u8, 5> const names{0, 'f', 'o', 'o', 0};
    std::memcpy(image.data() + STRING_OFFSET, names.data(), names.size());
    Dynamic32 needed{};
    needed.d_tag = static_cast<Traits32::Sword>(eftl::elf::DynTag::Needed);
    needed.d_val = 1;
    Dynamic32 flags{};
    flags.d_tag = static_cast<Traits32::Sword>(eftl::elf::DynTag::Flags);
    flags.d_val = 0x12345678U;
    Dynamic32 terminator{};
    terminator.d_tag = static_cast<Traits32::Sword>(eftl::elf::DynTag::DtNull);
    put(image, DYNAMIC_OFFSET, needed);
    put(image, DYNAMIC_OFFSET + sizeof(Dynamic32), flags);
    put(image, DYNAMIC_OFFSET + 2 * sizeof(Dynamic32), terminator);

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    auto const table = [&]() -> decltype(parsed->dynamic_table(2)) {
        if (parsed.is_ok()) {
            return parsed->dynamic_table(2);
        }
        return ustl::NONE;
    }();
    auto const flags_entry = [&]() -> decltype(table->entry_at(1)) {
        if (table.is_some()) {
            return table->entry_at(1);
        }
        return ustl::NONE;
    }();
    auto const needed_name = [&]() -> decltype(table->string_at(table->entry_at(0).value())) {
        if (table.is_some()) {
            return table->string_at(table->entry_at(0).value());
        }
        return ustl::NONE;
    }();
    auto const linkage = [&]() -> decltype(table->linkage()) {
        if (table.is_some()) {
            return table->linkage();
        }
        return ustl::NONE;
    }();
    auto const linked_needed = [&]() -> decltype(linkage->needed_library_at(0)) {
        if (linkage.is_some()) {
            return linkage->needed_library_at(0);
        }
        return ustl::NONE;
    }();
    if (table.is_some() && flags_entry.is_some() && needed_name.is_some() && linkage.is_some() &&
        linked_needed.is_some() && table->size() == 2 && linkage->needed_library_count() == 1 &&
        needed_name->size() == 3 && linked_needed->size() == 3 && linked_needed->data()[0] == 'f' &&
        eftl::elf::DynamicTableView<Traits32>::tag(flags_entry.value()) ==
            eftl::elf::DynTag::Flags &&
        eftl::elf::DynamicTableView<Traits32>::value(flags_entry.value()) == 0x12345678U &&
        linkage->flags().is_some() && linkage->flags().value() == 0x12345678U &&
        linkage->flags1().is_none()) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_dynamic_entries_and_names(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_invalid_dynamic_section_relationships(); result != 0) {
        return 10 + result;
    }
    if (test_elf32_observes_dynamic_entries() == 0) {
        return 0;
    }
    return 20;
}
