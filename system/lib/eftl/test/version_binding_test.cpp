/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/version_binding.hpp>

#include <array>

#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Section = Traits::Section;
using Symbol = Traits::Symbol;
using Version = Traits::Half;
using Definition = Traits::Verdef;
using DefinitionAuxiliary = Traits::Verdaux;
using Requirement = Traits::Verneed;
using RequirementAuxiliary = Traits::Vernaux;

constexpr usize SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize STRING_TABLE_OFFSET = 0x300;
constexpr usize DYNSYM_OFFSET = 0x340;
constexpr usize VERSYM_OFFSET = 0x3a0;
constexpr usize VERDEF_OFFSET = 0x3c0;
constexpr usize VERNEED_OFFSET = 0x400;

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

auto equals(eftl::ByteRange range, char const *text, usize size) -> bool {
    return range.size() == size && std::memcmp(range.data(), text, size) == 0;
}

auto make_version_binding_image() -> std::array<u8, 2048> {
    std::array<u8, 2048> image{};
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
    header.e_shnum = 6;
    put(image, 0, header);

    Section strings{};
    strings.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = STRING_TABLE_OFFSET;
    strings.sh_size = 39;
    strings.sh_addralign = 1;

    Section dynamic_symbols{};
    dynamic_symbols.sh_type = static_cast<u32>(eftl::elf::SectionType::Dynsym);
    dynamic_symbols.sh_offset = DYNSYM_OFFSET;
    dynamic_symbols.sh_size = 4 * sizeof(Symbol);
    dynamic_symbols.sh_link = 1;
    dynamic_symbols.sh_info = 1;
    dynamic_symbols.sh_addralign = alignof(Symbol);
    dynamic_symbols.sh_entsize = sizeof(Symbol);

    Section version_symbols{};
    version_symbols.sh_type = static_cast<u32>(eftl::elf::SectionType::GnuVersym);
    version_symbols.sh_offset = VERSYM_OFFSET;
    version_symbols.sh_size = 4 * sizeof(Version);
    version_symbols.sh_link = 2;
    version_symbols.sh_addralign = alignof(Version);
    version_symbols.sh_entsize = sizeof(Version);

    Section definitions{};
    definitions.sh_type = static_cast<u32>(eftl::elf::SectionType::GnuVerdef);
    definitions.sh_offset = VERDEF_OFFSET;
    definitions.sh_size = sizeof(Definition) + sizeof(DefinitionAuxiliary);
    definitions.sh_link = 1;
    definitions.sh_addralign = alignof(Definition);

    Section requirements{};
    requirements.sh_type = static_cast<u32>(eftl::elf::SectionType::GnuVerneed);
    requirements.sh_offset = VERNEED_OFFSET;
    requirements.sh_size = sizeof(Requirement) + sizeof(RequirementAuxiliary);
    requirements.sh_link = 1;
    requirements.sh_addralign = alignof(Requirement);

    put(image, SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put(image, SECTION_TABLE_OFFSET + 2 * sizeof(Section), dynamic_symbols);
    put(image, SECTION_TABLE_OFFSET + 3 * sizeof(Section), version_symbols);
    put(image, SECTION_TABLE_OFFSET + 4 * sizeof(Section), definitions);
    put(image, SECTION_TABLE_OFFSET + 5 * sizeof(Section), requirements);

    std::array<u8, 39> const names{
        0,   'f', 'o', 'o', 0,   'b', 'a', 'r', 0,   'b', 'a', 'z', 0,
        'V', 'E', 'R', '_', 'D', 'E', 'F', 0,   'l', 'i', 'b', 'd', 'e',
        'p', '.', 's', 'o', 0,   'V', 'E', 'R', '_', 'R', 'E', 'Q', 0,
    };
    std::memcpy(image.data() + STRING_TABLE_OFFSET, names.data(), names.size());

    Symbol definition_symbol{};
    definition_symbol.st_name = 5;
    put(image, DYNSYM_OFFSET + 2 * sizeof(Symbol), definition_symbol);
    Symbol requirement_symbol{};
    requirement_symbol.st_name = 9;
    put(image, DYNSYM_OFFSET + 3 * sizeof(Symbol), requirement_symbol);
    std::array<Version, 4> const versions{0, 1, 2, 0x8003U};
    std::memcpy(image.data() + VERSYM_OFFSET, versions.data(), sizeof(versions));

    Definition definition{};
    definition.vd_version = 1;
    definition.vd_ndx = 2;
    definition.vd_cnt = 1;
    definition.vd_aux = sizeof(Definition);
    put(image, VERDEF_OFFSET, definition);
    DefinitionAuxiliary definition_name{};
    definition_name.vda_name = 13;
    put(image, VERDEF_OFFSET + sizeof(Definition), definition_name);

    Requirement requirement{};
    requirement.vn_version = 1;
    requirement.vn_cnt = 1;
    requirement.vn_file = 21;
    requirement.vn_aux = sizeof(Requirement);
    put(image, VERNEED_OFFSET, requirement);
    RequirementAuxiliary requirement_name{};
    requirement_name.vna_other = 3;
    requirement_name.vna_name = 31;
    put(image, VERNEED_OFFSET + sizeof(Requirement), requirement_name);
    return image;
}

auto test_resolves_selected_version_sources() -> int {
    auto const image = make_version_binding_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const local = [&]() -> ustl::Option<eftl::elf::VersionBinding<Traits>> {
        if (parsed.is_ok()) {
            return parsed->version_binding(2, 3, 0);
        }
        return ustl::NONE;
    }();
    auto const global = [&]() -> ustl::Option<eftl::elf::VersionBinding<Traits>> {
        if (parsed.is_ok()) {
            return parsed->version_binding(2, 3, 1);
        }
        return ustl::NONE;
    }();
    auto const definition = [&]() -> ustl::Option<eftl::elf::VersionBinding<Traits>> {
        if (parsed.is_ok()) {
            return parsed->version_definition_binding(2, 3, 4, 2);
        }
        return ustl::NONE;
    }();
    auto const requirement = [&]() -> ustl::Option<eftl::elf::VersionBinding<Traits>> {
        if (parsed.is_ok()) {
            return parsed->version_requirement_binding(2, 3, 5, 3);
        }
        return ustl::NONE;
    }();
    if (local.is_some() && local->m_kind == eftl::elf::VersionBindingKind::Local &&
        local->m_index == 0 && !local->m_hidden && global.is_some() &&
        global->m_kind == eftl::elf::VersionBindingKind::Global && global->m_index == 1 &&
        !global->m_hidden && definition.is_some() &&
        definition->m_kind == eftl::elf::VersionBindingKind::Definition &&
        definition->m_index == 2 && !definition->m_hidden &&
        equals(definition->m_name, "VER_DEF", 7) && definition->m_provider.empty() &&
        requirement.is_some() &&
        requirement->m_kind == eftl::elf::VersionBindingKind::Requirement &&
        requirement->m_index == 3 && requirement->m_hidden &&
        equals(requirement->m_name, "VER_REQ", 7) &&
        equals(requirement->m_provider, "libdep.so", 9)) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_version_bindings() -> int {
    auto const image = make_version_binding_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->version_binding(1, 3, 0).is_some() ||
        parsed->version_definition_binding(2, 3, 5, 2).is_some() ||
        parsed->version_definition_binding(2, 3, 4, 3).is_some() ||
        parsed->version_requirement_binding(2, 3, 5, 2).is_some()) {
        return 2;
    }

    auto duplicate_definition = make_version_binding_image();
    Section definitions{};
    std::memcpy(&definitions,
                duplicate_definition.data() + SECTION_TABLE_OFFSET + 4 * sizeof(Section),
                sizeof(definitions));
    definitions.sh_size = 2 * (sizeof(Definition) + sizeof(DefinitionAuxiliary));
    put(duplicate_definition, SECTION_TABLE_OFFSET + 4 * sizeof(Section), definitions);
    Definition first_definition{};
    std::memcpy(&first_definition, duplicate_definition.data() + VERDEF_OFFSET,
                sizeof(first_definition));
    first_definition.vd_next = sizeof(Definition) + sizeof(DefinitionAuxiliary);
    put(duplicate_definition, VERDEF_OFFSET, first_definition);
    Definition second_definition{};
    second_definition.vd_version = 1;
    second_definition.vd_ndx = 2;
    second_definition.vd_cnt = 1;
    second_definition.vd_aux = sizeof(Definition);
    put(duplicate_definition, VERDEF_OFFSET + sizeof(Definition) + sizeof(DefinitionAuxiliary),
        second_definition);
    DefinitionAuxiliary second_definition_name{};
    second_definition_name.vda_name = 13;
    put(duplicate_definition, VERDEF_OFFSET + 2 * sizeof(Definition) + sizeof(DefinitionAuxiliary),
        second_definition_name);
    auto const duplicate_definition_parsed =
        Binary::from_raw(duplicate_definition.data(), duplicate_definition.size());
    if (duplicate_definition_parsed.is_err() ||
        duplicate_definition_parsed->version_definition_binding(2, 3, 4, 2).is_some()) {
        return 3;
    }

    auto duplicate_requirement = make_version_binding_image();
    Section requirements{};
    std::memcpy(&requirements,
                duplicate_requirement.data() + SECTION_TABLE_OFFSET + 5 * sizeof(Section),
                sizeof(requirements));
    requirements.sh_size = 2 * (sizeof(Requirement) + sizeof(RequirementAuxiliary));
    put(duplicate_requirement, SECTION_TABLE_OFFSET + 5 * sizeof(Section), requirements);
    Requirement first_requirement{};
    std::memcpy(&first_requirement, duplicate_requirement.data() + VERNEED_OFFSET,
                sizeof(first_requirement));
    first_requirement.vn_next = sizeof(Requirement) + sizeof(RequirementAuxiliary);
    put(duplicate_requirement, VERNEED_OFFSET, first_requirement);
    Requirement second_requirement{};
    second_requirement.vn_version = 1;
    second_requirement.vn_cnt = 1;
    second_requirement.vn_file = 21;
    second_requirement.vn_aux = sizeof(Requirement);
    put(duplicate_requirement, VERNEED_OFFSET + sizeof(Requirement) + sizeof(RequirementAuxiliary),
        second_requirement);
    RequirementAuxiliary second_requirement_name{};
    second_requirement_name.vna_other = 3;
    second_requirement_name.vna_name = 31;
    put(duplicate_requirement,
        VERNEED_OFFSET + 2 * sizeof(Requirement) + sizeof(RequirementAuxiliary),
        second_requirement_name);
    auto const duplicate_requirement_parsed =
        Binary::from_raw(duplicate_requirement.data(), duplicate_requirement.size());
    if (duplicate_requirement_parsed.is_ok() &&
        duplicate_requirement_parsed->version_requirement_binding(2, 3, 5, 3).is_none()) {
        return 0;
    }
    return 4;
}

auto test_elf32_resolves_version_sources() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Section32 = Traits32::Section;
    using Symbol32 = Traits32::Symbol;
    using Version32 = Traits32::Half;
    using Definition32 = Traits32::Verdef;
    using DefinitionAuxiliary32 = Traits32::Verdaux;
    using Requirement32 = Traits32::Verneed;
    using RequirementAuxiliary32 = Traits32::Vernaux;

    constexpr usize SECTION_OFFSET = sizeof(Header32);
    constexpr usize STRING_OFFSET = 0x200;
    constexpr usize DYNSYM_OFFSET_32 = 0x240;
    constexpr usize VERSYM_OFFSET_32 = 0x280;
    constexpr usize VERDEF_OFFSET_32 = 0x2a0;
    constexpr usize VERNEED_OFFSET_32 = 0x2e0;
    std::array<u8, 1024> image{};

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
    header.e_shnum = 6;
    put(image, 0, header);

    Section32 strings{};
    strings.sh_type = static_cast<u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = STRING_OFFSET;
    strings.sh_size = 35;
    strings.sh_addralign = 1;
    Section32 dynamic_symbols{};
    dynamic_symbols.sh_type = static_cast<u32>(eftl::elf::SectionType::Dynsym);
    dynamic_symbols.sh_offset = DYNSYM_OFFSET_32;
    dynamic_symbols.sh_size = 3 * sizeof(Symbol32);
    dynamic_symbols.sh_link = 1;
    dynamic_symbols.sh_info = 1;
    dynamic_symbols.sh_addralign = alignof(Symbol32);
    dynamic_symbols.sh_entsize = sizeof(Symbol32);
    Section32 version_symbols{};
    version_symbols.sh_type = static_cast<u32>(eftl::elf::SectionType::GnuVersym);
    version_symbols.sh_offset = VERSYM_OFFSET_32;
    version_symbols.sh_size = 3 * sizeof(Version32);
    version_symbols.sh_link = 2;
    version_symbols.sh_addralign = alignof(Version32);
    version_symbols.sh_entsize = sizeof(Version32);
    Section32 definitions{};
    definitions.sh_type = static_cast<u32>(eftl::elf::SectionType::GnuVerdef);
    definitions.sh_offset = VERDEF_OFFSET_32;
    definitions.sh_size = sizeof(Definition32) + sizeof(DefinitionAuxiliary32);
    definitions.sh_link = 1;
    definitions.sh_addralign = alignof(Definition32);
    Section32 requirements{};
    requirements.sh_type = static_cast<u32>(eftl::elf::SectionType::GnuVerneed);
    requirements.sh_offset = VERNEED_OFFSET_32;
    requirements.sh_size = sizeof(Requirement32) + sizeof(RequirementAuxiliary32);
    requirements.sh_link = 1;
    requirements.sh_addralign = alignof(Requirement32);
    put(image, SECTION_OFFSET + sizeof(Section32), strings);
    put(image, SECTION_OFFSET + 2 * sizeof(Section32), dynamic_symbols);
    put(image, SECTION_OFFSET + 3 * sizeof(Section32), version_symbols);
    put(image, SECTION_OFFSET + 4 * sizeof(Section32), definitions);
    put(image, SECTION_OFFSET + 5 * sizeof(Section32), requirements);

    std::array<u8, 35> const names{
        0,   'f', 'o', 'o', 0,   'b', 'a', 'r', 0, 'V', 'E', 'R', '_', 'D', 'E', 'F', 0, 'l',
        'i', 'b', 'd', 'e', 'p', '.', 's', 'o', 0, 'V', 'E', 'R', '_', 'R', 'E', 'Q', 0,
    };
    std::memcpy(image.data() + STRING_OFFSET, names.data(), names.size());
    Symbol32 definition_symbol{};
    definition_symbol.st_name = 1;
    put(image, DYNSYM_OFFSET_32 + sizeof(Symbol32), definition_symbol);
    Symbol32 requirement_symbol{};
    requirement_symbol.st_name = 5;
    put(image, DYNSYM_OFFSET_32 + 2 * sizeof(Symbol32), requirement_symbol);
    std::array<Version32, 3> const versions{0, 2, 0x8003U};
    std::memcpy(image.data() + VERSYM_OFFSET_32, versions.data(), sizeof(versions));

    Definition32 definition{};
    definition.vd_version = 1;
    definition.vd_ndx = 2;
    definition.vd_cnt = 1;
    definition.vd_aux = sizeof(Definition32);
    put(image, VERDEF_OFFSET_32, definition);
    DefinitionAuxiliary32 definition_name{};
    definition_name.vda_name = 9;
    put(image, VERDEF_OFFSET_32 + sizeof(Definition32), definition_name);

    Requirement32 requirement{};
    requirement.vn_version = 1;
    requirement.vn_cnt = 1;
    requirement.vn_file = 17;
    requirement.vn_aux = sizeof(Requirement32);
    put(image, VERNEED_OFFSET_32, requirement);
    RequirementAuxiliary32 requirement_name{};
    requirement_name.vna_other = 3;
    requirement_name.vna_name = 27;
    put(image, VERNEED_OFFSET_32 + sizeof(Requirement32), requirement_name);

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    auto const definition_binding = [&]() -> ustl::Option<eftl::elf::VersionBinding<Traits32>> {
        if (parsed.is_ok()) {
            return parsed->version_definition_binding(2, 3, 4, 1);
        }
        return ustl::NONE;
    }();
    auto const requirement_binding = [&]() -> ustl::Option<eftl::elf::VersionBinding<Traits32>> {
        if (parsed.is_ok()) {
            return parsed->version_requirement_binding(2, 3, 5, 2);
        }
        return ustl::NONE;
    }();
    if (definition_binding.is_some() &&
        definition_binding->m_kind == eftl::elf::VersionBindingKind::Definition &&
        equals(definition_binding->m_name, "VER_DEF", 7) && requirement_binding.is_some() &&
        requirement_binding->m_kind == eftl::elf::VersionBindingKind::Requirement &&
        requirement_binding->m_hidden && equals(requirement_binding->m_name, "VER_REQ", 7) &&
        equals(requirement_binding->m_provider, "libdep.so", 9)) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_resolves_selected_version_sources(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_invalid_version_bindings(); result != 0) {
        return 10 + result;
    }
    if (test_elf32_resolves_version_sources() == 0) {
        return 0;
    }
    return 20;
}
