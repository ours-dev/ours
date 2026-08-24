/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>

#include <array>

#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Segment = Traits::Segment;

constexpr u16 K_ET_REL = 1;
constexpr u16 K_ET_EXEC = 2;
constexpr u16 K_ET_DYN = 3;
constexpr u16 K_ET_CORE = 4;

auto make_image(u16 type, eftl::elf::SegPerms permissions, bool has_load = true)
    -> std::array<u8, 512> {
    std::array<u8, 512> bytes{};
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = type;
    header.e_version = 1;
    header.e_entry = 0x400108;
    header.e_ehsize = sizeof(Header);
    header.e_phoff = sizeof(Header);
    header.e_phentsize = sizeof(Segment);
    if (has_load) {
        header.e_phnum = 1;
    } else {
        header.e_phnum = 0;
    }
    std::memcpy(bytes.data(), &header, sizeof(header));

    if (!has_load) {
        return bytes;
    }
    Segment load{};
    load.p_type = static_cast<ours::u32>(eftl::elf::SegType::Load);
    load.p_flags = static_cast<ours::u32>(permissions);
    load.p_offset = 0x100;
    load.p_vaddr = 0x400100;
    load.p_filesz = 0x10;
    load.p_memsz = 0x20;
    load.p_align = 8;
    std::memcpy(bytes.data() + sizeof(Header), &load, sizeof(load));
    return bytes;
}

auto test_constructs_native_image_adapter() -> int {
    auto const bytes =
        make_image(K_ET_EXEC, eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    auto const parsed = Binary::from_raw(bytes.data(), bytes.size());
    auto const image = [&]() -> ustl::Option<eftl::elf::BasicImage<Traits>> {
        if (parsed.is_ok()) {
            return parsed->native_image();
        }
        return ustl::NONE;
    }();
    auto const offset = [&]() -> ustl::Option<usize> {
        if (image.is_some()) {
            return image->virtual_address_to_offset(0x400108);
        }
        return ustl::NONE;
    }();
    if (image.is_some() && image->format() == eftl::Binary::FormatId::Elf &&
        image->entrypoint() == 0x400108 && image->imagebase() == 0x400100 && offset.is_some() &&
        offset.value() == 0x108) {
        return 0;
    }
    return 1;
}

auto test_rejects_non_image_roles_and_invalid_contracts() -> int {
    auto const relocatable =
        make_image(K_ET_REL, eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    auto const relocatable_parsed = Binary::from_raw(relocatable.data(), relocatable.size());
    if (relocatable_parsed.is_err() || relocatable_parsed->native_image().is_some()) {
        return 1;
    }

    auto const core =
        make_image(K_ET_CORE, eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    auto const core_parsed = Binary::from_raw(core.data(), core.size());
    if (core_parsed.is_err() || core_parsed->native_image().is_some()) {
        return 2;
    }

    auto const no_load = make_image(K_ET_DYN, eftl::elf::SegPerms::Executable, false);
    auto const no_load_parsed = Binary::from_raw(no_load.data(), no_load.size());
    if (no_load_parsed.is_err() || no_load_parsed->native_image().is_some()) {
        return 3;
    }

    auto const non_executable = make_image(K_ET_EXEC, eftl::elf::SegPerms::Readable);
    auto const non_executable_parsed =
        Binary::from_raw(non_executable.data(), non_executable.size());
    if (non_executable_parsed.is_err() || non_executable_parsed->native_image().is_some()) {
        return 4;
    }

    auto overlapping =
        make_image(K_ET_EXEC, eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    Header header{};
    std::memcpy(&header, overlapping.data(), sizeof(header));
    header.e_phnum = 2;
    std::memcpy(overlapping.data(), &header, sizeof(header));
    Segment second_load{};
    std::memcpy(&second_load, overlapping.data() + sizeof(Header), sizeof(second_load));
    second_load.p_vaddr += 8;
    std::memcpy(overlapping.data() + sizeof(Header) + sizeof(Segment), &second_load,
                sizeof(second_load));
    auto const overlapping_parsed = Binary::from_raw(overlapping.data(), overlapping.size());
    if (overlapping_parsed.is_ok() && overlapping_parsed->native_image().is_none()) {
        return 0;
    }
    return 5;
}
} // namespace

auto main() -> int {
    if (auto const result = test_constructs_native_image_adapter(); result != 0) {
        return result;
    }
    if (test_rejects_non_image_roles_and_invalid_contracts() == 0) {
        return 0;
    }
    return 10;
}
