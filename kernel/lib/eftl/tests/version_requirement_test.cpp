/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/version_requirement.hpp>

#include <array>

#include <cstddef>
#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Section = Traits::Section;
using Requirement = Traits::Verneed;
using Auxiliary = Traits::Vernaux;

constexpr usize K_SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize K_STRING_TABLE_OFFSET = 0x200;
constexpr usize K_VERSION_REQUIREMENT_OFFSET = 0x240;

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

auto make_version_requirement_image() -> std::array<u8, 1024> {
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
    strings.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_TABLE_OFFSET;
    strings.sh_size = 27;
    strings.sh_addralign = 1;

    Section requirements{};
    requirements.sh_type = static_cast<u32>(eftl::elf::SectionType::GnuVerneed);
    requirements.sh_offset = K_VERSION_REQUIREMENT_OFFSET;
    requirements.sh_size = sizeof(Requirement) + 2 * sizeof(Auxiliary);
    requirements.sh_link = 1;
    requirements.sh_addralign = alignof(Requirement);

    put(image, K_SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), requirements);

    std::array<u8, 27> const names{0,   'l', 'i', 'b', 'f', 'o', 'o', '.', 's',
                                         'o', 0,   'V', 'E', 'R', '_', '2', 0,   'V',
                                         'E', 'R', '_', 'A', 'L', 'I', 'A', 'S', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());

    Requirement requirement{};
    requirement.vn_version = 1;
    requirement.vn_cnt = 2;
    requirement.vn_file = 1;
    requirement.vn_aux = sizeof(Requirement);
    put(image, K_VERSION_REQUIREMENT_OFFSET, requirement);

    Auxiliary primary{};
    primary.vna_other = 2;
    primary.vna_name = 11;
    primary.vna_next = sizeof(Auxiliary);
    put(image, K_VERSION_REQUIREMENT_OFFSET + sizeof(Requirement), primary);

    Auxiliary alias{};
    alias.vna_other = 0x8003U;
    alias.vna_name = 17;
    put(image, K_VERSION_REQUIREMENT_OFFSET + sizeof(Requirement) + sizeof(Auxiliary), alias);
    return image;
}

auto make_big_endian_version_requirement_image() -> std::array<u8, 1024> {
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
    constexpr usize K_REQUIREMENT_SECTION = K_SECTION_TABLE_OFFSET + 2 * sizeof(Section);
    put_big_endian_u32(image, K_STRING_SECTION + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::Strtab));
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_offset),
                       K_STRING_TABLE_OFFSET);
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_size), 27);
    put_big_endian_u64(image, K_STRING_SECTION + offsetof(Section, sh_addralign), 1);

    put_big_endian_u32(image, K_REQUIREMENT_SECTION + offsetof(Section, sh_type),
                       static_cast<u32>(eftl::elf::SectionType::GnuVerneed));
    put_big_endian_u64(image, K_REQUIREMENT_SECTION + offsetof(Section, sh_offset),
                       K_VERSION_REQUIREMENT_OFFSET);
    put_big_endian_u64(image, K_REQUIREMENT_SECTION + offsetof(Section, sh_size),
                       sizeof(Requirement) + 2 * sizeof(Auxiliary));
    put_big_endian_u32(image, K_REQUIREMENT_SECTION + offsetof(Section, sh_link), 1);
    put_big_endian_u64(image, K_REQUIREMENT_SECTION + offsetof(Section, sh_addralign),
                       alignof(Requirement));

    std::array<u8, 27> const names{0,   'l', 'i', 'b', 'f', 'o', 'o', '.', 's',
                                         'o', 0,   'V', 'E', 'R', '_', '2', 0,   'V',
                                         'E', 'R', '_', 'A', 'L', 'I', 'A', 'S', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());
    put_big_endian_u16(image, K_VERSION_REQUIREMENT_OFFSET + offsetof(Requirement, vn_version), 1);
    put_big_endian_u16(image, K_VERSION_REQUIREMENT_OFFSET + offsetof(Requirement, vn_cnt), 2);
    put_big_endian_u32(image, K_VERSION_REQUIREMENT_OFFSET + offsetof(Requirement, vn_file), 1);
    put_big_endian_u32(image, K_VERSION_REQUIREMENT_OFFSET + offsetof(Requirement, vn_aux),
                       sizeof(Requirement));
    constexpr usize K_PRIMARY_OFFSET = K_VERSION_REQUIREMENT_OFFSET + sizeof(Requirement);
    constexpr usize K_ALIAS_OFFSET = K_PRIMARY_OFFSET + sizeof(Auxiliary);
    put_big_endian_u16(image, K_PRIMARY_OFFSET + offsetof(Auxiliary, vna_other), 2);
    put_big_endian_u32(image, K_PRIMARY_OFFSET + offsetof(Auxiliary, vna_name), 11);
    put_big_endian_u32(image, K_PRIMARY_OFFSET + offsetof(Auxiliary, vna_next), sizeof(Auxiliary));
    put_big_endian_u16(image, K_ALIAS_OFFSET + offsetof(Auxiliary, vna_other), 0x8003U);
    put_big_endian_u32(image, K_ALIAS_OFFSET + offsetof(Auxiliary, vna_name), 17);
    return image;
}

auto equals(eftl::ByteRange const &range, char const *text, usize size) -> bool {
    return range.size() == size && std::memcmp(range.data(), text, size) == 0;
}

auto test_observes_version_requirements() -> int {
    auto const image = make_version_requirement_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const requirements = [&]() -> decltype(parsed->version_requirement_table(2)) {
        if (parsed.is_ok()) {
            return parsed->version_requirement_table(2);
        }
        return ustl::NONE;
    }();
    auto const requirement = [&]() -> decltype(requirements->requirement_at(0)) {
        if (requirements.is_some()) {
            return requirements->requirement_at(0);
        }
        return ustl::NONE;
    }();
    auto const file_name = [&]() -> decltype(requirements->file_name(0)) {
        if (requirements.is_some()) {
            return requirements->file_name(0);
        }
        return ustl::NONE;
    }();
    auto const version_name = [&]() -> decltype(requirements->auxiliary_name(0, 1)) {
        if (requirements.is_some()) {
            return requirements->auxiliary_name(0, 1);
        }
        return ustl::NONE;
    }();
    auto const auxiliary = [&]() -> decltype(requirements->auxiliary_at(0, 1)) {
        if (requirements.is_some()) {
            return requirements->auxiliary_at(0, 1);
        }
        return ustl::NONE;
    }();
    if (requirements.is_some() && requirements->size() == 1 && requirement.is_some() &&
        requirement->vn_cnt == 2 && file_name.is_some() && version_name.is_some() &&
        auxiliary.is_some() && equals(file_name.value(), "libfoo.so", 9) &&
        equals(version_name.value(), "VER_ALIAS", 9) &&
        eftl::elf::VersionRequirementTableView<Traits>::version_index(auxiliary->vna_other) == 3 &&
        eftl::elf::VersionRequirementTableView<Traits>::is_hidden(auxiliary->vna_other) &&
        requirements->auxiliary_at(0, 2).is_none() && requirements->requirement_at(1).is_none() &&
        parsed->version_requirement_table(1).is_none()) {
        return 0;
    }
    return 1;
}

auto test_observes_big_endian_version_requirement() -> int {
    auto const image = make_big_endian_version_requirement_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const requirements = [&]() -> decltype(parsed->version_requirement_table(2)) {
        if (parsed.is_ok()) {
            return parsed->version_requirement_table(2);
        }
        return ustl::NONE;
    }();
    auto const requirement = [&]() -> decltype(requirements->requirement_at(0)) {
        if (requirements.is_some()) {
            return requirements->requirement_at(0);
        }
        return ustl::NONE;
    }();
    auto const file_name = [&]() -> decltype(requirements->file_name(0)) {
        if (requirements.is_some()) {
            return requirements->file_name(0);
        }
        return ustl::NONE;
    }();
    auto const version_name = [&]() -> decltype(requirements->auxiliary_name(0, 1)) {
        if (requirements.is_some()) {
            return requirements->auxiliary_name(0, 1);
        }
        return ustl::NONE;
    }();
    auto const alias = [&]() -> decltype(requirements->auxiliary_at(0, 1)) {
        if (requirements.is_some()) {
            return requirements->auxiliary_at(0, 1);
        }
        return ustl::NONE;
    }();
    if (requirement.is_some() && file_name.is_some() && version_name.is_some() && alias.is_some() &&
        requirement->vn_cnt == 2 && equals(file_name.value(), "libfoo.so", 9) &&
        equals(version_name.value(), "VER_ALIAS", 9) &&
        eftl::elf::VersionRequirementTableView<Traits>::version_index(alias->vna_other) == 3 &&
        eftl::elf::VersionRequirementTableView<Traits>::is_hidden(alias->vna_other)) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_version_requirements() -> int {
    auto wrong_link = make_version_requirement_image();
    Section requirements{};
    std::memcpy(&requirements, wrong_link.data() + K_SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(requirements));
    requirements.sh_link = 0;
    put(wrong_link, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), requirements);
    auto const wrong_link_result = Binary::from_raw(wrong_link.data(), wrong_link.size());
    if (wrong_link_result.is_ok() ||
        wrong_link_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 1;
    }

    auto unknown_revision = make_version_requirement_image();
    Requirement requirement{};
    std::memcpy(&requirement, unknown_revision.data() + K_VERSION_REQUIREMENT_OFFSET,
                sizeof(requirement));
    requirement.vn_version = 2;
    put(unknown_revision, K_VERSION_REQUIREMENT_OFFSET, requirement);
    auto const unknown_revision_result =
        Binary::from_raw(unknown_revision.data(), unknown_revision.size());
    if (unknown_revision_result.is_ok() ||
        unknown_revision_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 2;
    }

    auto invalid_auxiliary_offset = make_version_requirement_image();
    std::memcpy(&requirement, invalid_auxiliary_offset.data() + K_VERSION_REQUIREMENT_OFFSET,
                sizeof(requirement));
    requirement.vn_aux = sizeof(Requirement) - 1;
    put(invalid_auxiliary_offset, K_VERSION_REQUIREMENT_OFFSET, requirement);
    auto const invalid_auxiliary_offset_result =
        Binary::from_raw(invalid_auxiliary_offset.data(), invalid_auxiliary_offset.size());
    if (invalid_auxiliary_offset_result.is_ok() ||
        invalid_auxiliary_offset_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 3;
    }

    auto overlapping_next_requirement = make_version_requirement_image();
    std::memcpy(&requirement, overlapping_next_requirement.data() + K_VERSION_REQUIREMENT_OFFSET,
                sizeof(requirement));
    requirement.vn_next = sizeof(Requirement);
    put(overlapping_next_requirement, K_VERSION_REQUIREMENT_OFFSET, requirement);
    auto const overlapping_next_requirement_result =
        Binary::from_raw(overlapping_next_requirement.data(), overlapping_next_requirement.size());
    if (overlapping_next_requirement_result.is_ok() ||
        overlapping_next_requirement_result.unwrap_err() != eftl::elf::ParseError::InvalidVersion) {
        return 4;
    }

    auto unterminated_auxiliaries = make_version_requirement_image();
    Auxiliary alias{};
    std::memcpy(&alias,
                unterminated_auxiliaries.data() + K_VERSION_REQUIREMENT_OFFSET +
                    sizeof(Requirement) + sizeof(Auxiliary),
                sizeof(alias));
    alias.vna_next = sizeof(Auxiliary);
    put(unterminated_auxiliaries,
        K_VERSION_REQUIREMENT_OFFSET + sizeof(Requirement) + sizeof(Auxiliary), alias);
    auto const unterminated_auxiliaries_result =
        Binary::from_raw(unterminated_auxiliaries.data(), unterminated_auxiliaries.size());
    if (unterminated_auxiliaries_result.is_err() &&
        unterminated_auxiliaries_result.unwrap_err() == eftl::elf::ParseError::InvalidVersion) {
        return 0;
    }
    return 5;
}

auto test_observes_linked_version_requirements() -> int {
    auto image = make_version_requirement_image();
    Section requirements{};
    std::memcpy(&requirements, image.data() + K_SECTION_TABLE_OFFSET + 2 * sizeof(Section),
                sizeof(requirements));
    requirements.sh_size = 2 * (sizeof(Requirement) + sizeof(Auxiliary));
    put(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), requirements);

    Requirement first{};
    std::memcpy(&first, image.data() + K_VERSION_REQUIREMENT_OFFSET, sizeof(first));
    first.vn_cnt = 1;
    first.vn_next = sizeof(Requirement) + sizeof(Auxiliary);
    put(image, K_VERSION_REQUIREMENT_OFFSET, first);
    Auxiliary primary{};
    std::memcpy(&primary, image.data() + K_VERSION_REQUIREMENT_OFFSET + sizeof(Requirement),
                sizeof(primary));
    primary.vna_next = 0;
    put(image, K_VERSION_REQUIREMENT_OFFSET + sizeof(Requirement), primary);

    Requirement second{};
    second.vn_version = 1;
    second.vn_cnt = 1;
    second.vn_file = 1;
    second.vn_aux = sizeof(Requirement);
    put(image, K_VERSION_REQUIREMENT_OFFSET + sizeof(Requirement) + sizeof(Auxiliary), second);
    Auxiliary alias{};
    alias.vna_other = 3;
    alias.vna_name = 17;
    put(image, K_VERSION_REQUIREMENT_OFFSET + 2 * sizeof(Requirement) + sizeof(Auxiliary), alias);

    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const version_requirements = [&]() -> decltype(parsed->version_requirement_table(2)) {
        if (parsed.is_ok()) {
            return parsed->version_requirement_table(2);
        }
        return ustl::NONE;
    }();
    auto const second_requirement = [&]() -> decltype(version_requirements->requirement_at(1)) {
        if (version_requirements.is_some()) {
            return version_requirements->requirement_at(1);
        }
        return ustl::NONE;
    }();
    auto const second_name = [&]() -> decltype(version_requirements->auxiliary_name(1, 0)) {
        if (version_requirements.is_some()) {
            return version_requirements->auxiliary_name(1, 0);
        }
        return ustl::NONE;
    }();
    if (version_requirements.is_some() && version_requirements->size() == 2 &&
        second_requirement.is_some() && second_requirement->vn_cnt == 1 && second_name.is_some() &&
        equals(second_name.value(), "VER_ALIAS", 9)) {
        return 0;
    }
    return 1;
}

auto test_elf32_observes_version_requirement() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Section32 = Traits32::Section;
    using Requirement32 = Traits32::Verneed;
    using Auxiliary32 = Traits32::Vernaux;

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
    strings.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_OFFSET;
    strings.sh_size = 15;
    strings.sh_addralign = 1;
    Section32 requirements{};
    requirements.sh_type = static_cast<u32>(eftl::elf::SectionType::GnuVerneed);
    requirements.sh_offset = K_VERSION_OFFSET;
    requirements.sh_size = sizeof(Requirement32) + sizeof(Auxiliary32);
    requirements.sh_link = 1;
    requirements.sh_addralign = alignof(Requirement32);
    put(image, K_SECTION_OFFSET + sizeof(Section32), strings);
    put(image, K_SECTION_OFFSET + 2 * sizeof(Section32), requirements);

    std::array<u8, 15> const names{0, 'l', 'i', 'b', 'x', '.', 's', 'o',
                                         0, 'V', 'E', 'R', '_', '4', 0};
    std::memcpy(image.data() + K_STRING_OFFSET, names.data(), names.size());
    Requirement32 requirement{};
    requirement.vn_version = 1;
    requirement.vn_cnt = 1;
    requirement.vn_file = 1;
    requirement.vn_aux = sizeof(Requirement32);
    put(image, K_VERSION_OFFSET, requirement);
    Auxiliary32 auxiliary{};
    auxiliary.vna_other = 0x8004U;
    auxiliary.vna_name = 9;
    put(image, K_VERSION_OFFSET + sizeof(Requirement32), auxiliary);

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    auto const version_requirements = [&]() -> decltype(parsed->version_requirement_table(2)) {
        if (parsed.is_ok()) {
            return parsed->version_requirement_table(2);
        }
        return ustl::NONE;
    }();
    auto const file_name = [&]() -> decltype(version_requirements->file_name(0)) {
        if (version_requirements.is_some()) {
            return version_requirements->file_name(0);
        }
        return ustl::NONE;
    }();
    auto const requirement_auxiliary = [&]() -> decltype(version_requirements->auxiliary_at(0, 0)) {
        if (version_requirements.is_some()) {
            return version_requirements->auxiliary_at(0, 0);
        }
        return ustl::NONE;
    }();
    if (file_name.is_some() && requirement_auxiliary.is_some() &&
        equals(file_name.value(), "libx.so", 7) &&
        eftl::elf::VersionRequirementTableView<Traits32>::version_index(
            requirement_auxiliary->vna_other) == 4 &&
        eftl::elf::VersionRequirementTableView<Traits32>::is_hidden(
            requirement_auxiliary->vna_other)) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_version_requirements(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_invalid_version_requirements(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_observes_linked_version_requirements(); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_observes_big_endian_version_requirement(); result != 0) {
        return 30 + result;
    }
    if (test_elf32_observes_version_requirement() == 0) {
        return 0;
    }
    return 40;
}
