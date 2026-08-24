/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/version_definition.hpp>

#include <array>

#include <cstddef>
#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Section = Traits::Section;
using Definition = Traits::Verdef;
using Auxiliary = Traits::Verdaux;

constexpr usize K_SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize K_STRING_TABLE_OFFSET = 0x200;
constexpr usize K_VERSION_DEFINITION_OFFSET = 0x240;

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
auto put_big_endian_u32(std::array<u8, Size> &image, usize offset, ours::u32 value)
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

auto make_version_definition_image() -> std::array<u8, 1024> {
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
    header.e_shnum = 3;
    put(image, 0, header);

    Section strings{};
    strings.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_TABLE_OFFSET;
    strings.sh_size = 17;
    strings.sh_addralign = 1;

    Section definitions{};
    definitions.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::GnuVerdef);
    definitions.sh_offset = K_VERSION_DEFINITION_OFFSET;
    definitions.sh_size = sizeof(Definition) + 2 * sizeof(Auxiliary);
    definitions.sh_link = 1;
    definitions.sh_addralign = alignof(Definition);

    put(image, K_SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), definitions);

    std::array<u8, 17> const names{0,   'V', 'E', 'R', '_', '1', 0,   'V', 'E',
                                         'R', '_', 'A', 'L', 'I', 'A', 'S', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());

    Definition definition{};
    definition.vd_version = 1;
    definition.vd_ndx = 2;
    definition.vd_cnt = 2;
    definition.vd_aux = sizeof(Definition);
    put(image, K_VERSION_DEFINITION_OFFSET, definition);

    Auxiliary primary{};
    primary.vda_name = 1;
    primary.vda_next = sizeof(Auxiliary);
    put(image, K_VERSION_DEFINITION_OFFSET + sizeof(Definition), primary);

    Auxiliary alias{};
    alias.vda_name = 7;
    put(image, K_VERSION_DEFINITION_OFFSET + sizeof(Definition) + sizeof(Auxiliary), alias);
    return image;
}

auto make_big_endian_version_definition_image() -> std::array<u8, 1024> {
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
    put_big_endian_u16(image, offsetof(Header, e_shnum), 3);

    constexpr usize K_STRING_SECTION = K_SECTION_TABLE_OFFSET + sizeof(Section);
    constexpr usize K_DEFINITION_SECTION = K_SECTION_TABLE_OFFSET + 2 * sizeof(Section);
    put_big_endian_u32(image, K_STRING_SECTION + offsetof(Section, sh_type),
                       static_cast<ours::u32>(eftl::elf::SectionType::Strtab));
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_offset),
                       K_STRING_TABLE_OFFSET);
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_size), 17);
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_addralign), 1);

    put_big_endian_u32(image, K_DEFINITION_SECTION + offsetof(Section, sh_type),
                       static_cast<ours::u32>(eftl::elf::SectionType::GnuVerdef));
    put_big_endian_u64(image, K_DEFINITION_SECTION + offsetof(Section, sh_offset),
                       K_VERSION_DEFINITION_OFFSET);
    put_big_endian_u64(image, K_DEFINITION_SECTION + offsetof(Section, sh_size),
                       sizeof(Definition) + 2 * sizeof(Auxiliary));
    put_big_endian_u32(image, K_DEFINITION_SECTION + offsetof(Section, sh_link), 1);
    put_big_endian_u64(image, K_DEFINITION_SECTION + offsetof(Section, sh_addralign),
                       alignof(Definition));

    std::array<u8, 17> const names{0,   'V', 'E', 'R', '_', '1', 0,   'V', 'E',
                                         'R', '_', 'A', 'L', 'I', 'A', 'S', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());
    put_big_endian_u16(image, K_VERSION_DEFINITION_OFFSET + offsetof(Definition, vd_version), 1);
    put_big_endian_u16(image, K_VERSION_DEFINITION_OFFSET + offsetof(Definition, vd_ndx), 2);
    put_big_endian_u16(image, K_VERSION_DEFINITION_OFFSET + offsetof(Definition, vd_cnt), 2);
    put_big_endian_u32(image, K_VERSION_DEFINITION_OFFSET + offsetof(Definition, vd_aux),
                       sizeof(Definition));
    constexpr usize K_PRIMARY_OFFSET = K_VERSION_DEFINITION_OFFSET + sizeof(Definition);
    constexpr usize K_ALIAS_OFFSET = K_PRIMARY_OFFSET + sizeof(Auxiliary);
    put_big_endian_u32(image, K_PRIMARY_OFFSET + offsetof(Auxiliary, vda_name), 1);
    put_big_endian_u32(image, K_PRIMARY_OFFSET + offsetof(Auxiliary, vda_next), sizeof(Auxiliary));
    put_big_endian_u32(image, K_ALIAS_OFFSET + offsetof(Auxiliary, vda_name), 7);
    return image;
}

auto equals(eftl::ByteRange const &range, char const *text, usize size) -> bool {
    return range.size() == size && std::memcmp(range.data(), text, size) == 0;
}

auto test_observes_version_definition() -> int {
    auto const image = make_version_definition_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const definitions = [&]() -> decltype(parsed->version_definition_table(2)) {
        if (parsed.is_ok()) {
            return parsed->version_definition_table(2);
        }
        return ustl::NONE;
    }();
    auto const definition = [&]() -> decltype(definitions->definition_at(0)) {
        if (definitions.is_some()) {
            return definitions->definition_at(0);
        }
        return ustl::NONE;
    }();
    auto const primary = [&]() -> decltype(definitions->definition_name(0)) {
        if (definitions.is_some()) {
            return definitions->definition_name(0);
        }
        return ustl::NONE;
    }();
    auto const alias = [&]() -> decltype(definitions->auxiliary_name(0, 1)) {
        if (definitions.is_some()) {
            return definitions->auxiliary_name(0, 1);
        }
        return ustl::NONE;
    }();
    if (definitions.is_some() && definitions->size() == 1 && definition.is_some() &&
        definition->vd_ndx == 2 && definition->vd_cnt == 2 && primary.is_some() &&
        alias.is_some() && equals(primary.value(), "VER_1", 5) &&
        equals(alias.value(), "VER_ALIAS", 9) && definitions->auxiliary_at(0, 2).is_none() &&
        definitions->definition_at(1).is_none() && parsed->version_definition_table(1).is_none()) {
        return 0;
    }
    return 1;
}

auto test_observes_big_endian_version_definition() -> int {
    auto const image = make_big_endian_version_definition_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const definitions = [&]() -> decltype(parsed->version_definition_table(2)) {
        if (parsed.is_ok()) {
            return parsed->version_definition_table(2);
        }
        return ustl::NONE;
    }();
    auto const definition = [&]() -> decltype(definitions->definition_at(0)) {
        if (definitions.is_some()) {
            return definitions->definition_at(0);
        }
        return ustl::NONE;
    }();
    auto const primary = [&]() -> decltype(definitions->definition_name(0)) {
        if (definitions.is_some()) {
            return definitions->definition_name(0);
        }
        return ustl::NONE;
    }();
    auto const alias = [&]() -> decltype(definitions->auxiliary_name(0, 1)) {
        if (definitions.is_some()) {
            return definitions->auxiliary_name(0, 1);
        }
        return ustl::NONE;
    }();
    if (definition.is_some() && primary.is_some() && alias.is_some() && definition->vd_ndx == 2 &&
        definition->vd_cnt == 2 && equals(primary.value(), "VER_1", 5) &&
        equals(alias.value(), "VER_ALIAS", 9)) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_version_definitions() -> int {
    auto wrong_link = make_version_definition_image();
    Section definitions{};
    std::memcpy(&definitions, wrong_link.data() + K_SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(definitions));
    definitions.sh_link = 0;
    put(wrong_link, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), definitions);
    auto const wrong_link_result = Binary::from_raw(wrong_link.data(), wrong_link.size());
    if (wrong_link_result.is_ok() ||
        wrong_link_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 1;
    }

    auto unknown_revision = make_version_definition_image();
    Definition definition{};
    std::memcpy(&definition, unknown_revision.data() + K_VERSION_DEFINITION_OFFSET,
                sizeof(definition));
    definition.vd_version = 2;
    put(unknown_revision, K_VERSION_DEFINITION_OFFSET, definition);
    auto const unknown_revision_result =
        Binary::from_raw(unknown_revision.data(), unknown_revision.size());
    if (unknown_revision_result.is_ok() ||
        unknown_revision_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 2;
    }

    auto invalid_auxiliary_offset = make_version_definition_image();
    std::memcpy(&definition, invalid_auxiliary_offset.data() + K_VERSION_DEFINITION_OFFSET,
                sizeof(definition));
    definition.vd_aux = sizeof(Definition) - 1;
    put(invalid_auxiliary_offset, K_VERSION_DEFINITION_OFFSET, definition);
    auto const invalid_auxiliary_offset_result =
        Binary::from_raw(invalid_auxiliary_offset.data(), invalid_auxiliary_offset.size());
    if (invalid_auxiliary_offset_result.is_ok() ||
        invalid_auxiliary_offset_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 3;
    }

    auto overlapping_next_definition = make_version_definition_image();
    std::memcpy(&definition, overlapping_next_definition.data() + K_VERSION_DEFINITION_OFFSET,
                sizeof(definition));
    definition.vd_next = sizeof(Definition);
    put(overlapping_next_definition, K_VERSION_DEFINITION_OFFSET, definition);
    auto const overlapping_next_definition_result =
        Binary::from_raw(overlapping_next_definition.data(), overlapping_next_definition.size());
    if (overlapping_next_definition_result.is_ok() ||
        overlapping_next_definition_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 4;
    }

    auto unterminated_auxiliaries = make_version_definition_image();
    Auxiliary alias{};
    std::memcpy(&alias,
                unterminated_auxiliaries.data() + K_VERSION_DEFINITION_OFFSET + sizeof(Definition) +
                    sizeof(Auxiliary),
                sizeof(alias));
    alias.vda_next = sizeof(Auxiliary);
    put(unterminated_auxiliaries,
        K_VERSION_DEFINITION_OFFSET + sizeof(Definition) + sizeof(Auxiliary), alias);
    auto const unterminated_auxiliaries_result =
        Binary::from_raw(unterminated_auxiliaries.data(), unterminated_auxiliaries.size());
    if (unterminated_auxiliaries_result.is_err() &&
        unterminated_auxiliaries_result.unwrap_err() == eftl::elf::ParseError::InvalidVersion) {
        return 0;
    }
    return 5;
}

auto test_observes_linked_version_definitions() -> int {
    auto image = make_version_definition_image();
    Section definitions{};
    std::memcpy(&definitions, image.data() + K_SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(definitions));
    definitions.sh_size = 2 * (sizeof(Definition) + sizeof(Auxiliary));
    put(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), definitions);

    Definition first{};
    std::memcpy(&first, image.data() + K_VERSION_DEFINITION_OFFSET, sizeof(first));
    first.vd_cnt = 1;
    first.vd_next = sizeof(Definition) + sizeof(Auxiliary);
    put(image, K_VERSION_DEFINITION_OFFSET, first);
    Auxiliary primary{};
    std::memcpy(&primary, image.data() + K_VERSION_DEFINITION_OFFSET + sizeof(Definition),
                sizeof(primary));
    primary.vda_next = 0;
    put(image, K_VERSION_DEFINITION_OFFSET + sizeof(Definition), primary);

    Definition second{};
    second.vd_version = 1;
    second.vd_ndx = 3;
    second.vd_cnt = 1;
    second.vd_aux = sizeof(Definition);
    put(image, K_VERSION_DEFINITION_OFFSET + sizeof(Definition) + sizeof(Auxiliary), second);
    Auxiliary alias{};
    alias.vda_name = 7;
    put(image, K_VERSION_DEFINITION_OFFSET + 2 * sizeof(Definition) + sizeof(Auxiliary), alias);

    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const version_definitions = [&]() -> decltype(parsed->version_definition_table(2)) {
        if (parsed.is_ok()) {
            return parsed->version_definition_table(2);
        }
        return ustl::NONE;
    }();
    auto const second_definition = [&]() -> decltype(version_definitions->definition_at(1)) {
        if (version_definitions.is_some()) {
            return version_definitions->definition_at(1);
        }
        return ustl::NONE;
    }();
    auto const second_name = [&]() -> decltype(version_definitions->definition_name(1)) {
        if (version_definitions.is_some()) {
            return version_definitions->definition_name(1);
        }
        return ustl::NONE;
    }();
    if (version_definitions.is_some() && version_definitions->size() == 2 &&
        second_definition.is_some() && second_definition->vd_ndx == 3 && second_name.is_some() &&
        equals(second_name.value(), "VER_ALIAS", 9)) {
        return 0;
    }
    return 1;
}

auto test_elf32_observes_version_definition() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Section32 = Traits32::Section;
    using Definition32 = Traits32::Verdef;
    using Auxiliary32 = Traits32::Verdaux;

    constexpr usize K_SECTION_OFFSET = sizeof(Header32);
    constexpr usize K_STRING_OFFSET = 0x100;
    constexpr usize K_VERSION_OFFSET = 0x140;
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
    header.e_shnum = 3;
    put(image, 0, header);

    Section32 strings{};
    strings.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_OFFSET;
    strings.sh_size = 7;
    strings.sh_addralign = 1;
    Section32 definitions{};
    definitions.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::GnuVerdef);
    definitions.sh_offset = K_VERSION_OFFSET;
    definitions.sh_size = sizeof(Definition32) + sizeof(Auxiliary32);
    definitions.sh_link = 1;
    definitions.sh_addralign = alignof(Definition32);
    put(image, K_SECTION_OFFSET + sizeof(Section32), strings);
    put(image, K_SECTION_OFFSET + 2 * sizeof(Section32), definitions);

    std::array<u8, 7> const names{0, 'V', 'E', 'R', '_', '3', 0};
    std::memcpy(image.data() + K_STRING_OFFSET, names.data(), names.size());
    Definition32 definition{};
    definition.vd_version = 1;
    definition.vd_ndx = 3;
    definition.vd_cnt = 1;
    definition.vd_aux = sizeof(Definition32);
    put(image, K_VERSION_OFFSET, definition);
    Auxiliary32 auxiliary{};
    auxiliary.vda_name = 1;
    put(image, K_VERSION_OFFSET + sizeof(Definition32), auxiliary);

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    auto const version_definitions = [&]() -> decltype(parsed->version_definition_table(2)) {
        if (parsed.is_ok()) {
            return parsed->version_definition_table(2);
        }
        return ustl::NONE;
    }();
    auto const name = [&]() -> decltype(version_definitions->definition_name(0)) {
        if (version_definitions.is_some()) {
            return version_definitions->definition_name(0);
        }
        return ustl::NONE;
    }();
    if (version_definitions.is_some() && version_definitions->size() == 1 && name.is_some() &&
        equals(name.value(), "VER_3", 5)) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_version_definition(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_invalid_version_definitions(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_observes_linked_version_definitions(); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_observes_big_endian_version_definition(); result != 0) {
        return 30 + result;
    }
    if (test_elf32_observes_version_definition() == 0) {
        return 0;
    }
    return 40;
}
