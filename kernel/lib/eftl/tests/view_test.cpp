/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/views.hpp>

#include <array>

#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Header = Traits::Header;
using Segment = Traits::Segment;
using Dyn = Traits::Dyn;

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

template <typename T, usize Size>
auto get(std::array<u8, Size> const &image, usize offset) -> T {
    T value{};
    std::memcpy(&value, image.data() + offset, sizeof(value));
    return value;
}

auto make_header(u16 type, u16 program_count) -> Header {
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = type;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_entry = 0x1000;
    header.e_phoff = sizeof(Header);
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = program_count;
    header.e_shentsize = sizeof(Traits::Section);
    return header;
}

auto make_load_segment() -> Segment {
    Segment segment{};
    segment.p_type = static_cast<u32>(eftl::elf::SegType::Load);
    segment.p_flags =
        static_cast<u32>(eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    segment.p_vaddr = 0x1000;
    segment.p_memsz = 0x1000;
    segment.p_align = 0x1000;
    return segment;
}

auto make_image() -> std::array<u8, 512> {
    std::array<u8, 512> image{};
    auto const header = make_header(2, 1);
    auto const segment = make_load_segment();
    put(image, 0, header);
    put(image, header.e_phoff, segment);
    return image;
}

auto test_valid_image() -> int {
    alignas(Segment) std::array<u8, 512> const image = make_image();
    auto const parsed = eftl::elf::View<Traits>::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1 + static_cast<int>(parsed.unwrap_err());
    }
    if (parsed->segments().size() != 1 || !parsed->sections().empty()) {
        return 2;
    }
    if (parsed->virtual_size() != 0x1000 || parsed->virtual_base() != 0x1000) {
        return 3;
    }
    auto const mapped = parsed->virt_addr_to_offset(0x1000);
    if (mapped.is_none() || mapped.value() != 0) {
        return 4;
    }
    if (parsed->virt_addr_to_offset(0x2000).is_some()) {
        return 5;
    }
    return 0;
}

auto test_unaligned_valid_input() -> int {
    auto const image = make_image();
    alignas(Segment) std::array<u8, image.size() + 1> storage{};
    std::memcpy(storage.data() + 1, image.data(), image.size());

    auto const parsed = eftl::elf::View<Traits>::from_raw(storage.data() + 1, image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->segments().size() != 1 ||
        parsed->segments()[0].type() != eftl::elf::SegType::Load) {
        return 2;
    }
    return 0;
}

auto test_rejects_truncated_program_table() -> int {
    auto image = make_image();
    auto header = get<Header>(image, 0);
    header.e_phoff = image.size() - sizeof(Segment) + 1;
    put(image, 0, header);

    auto const parsed = eftl::elf::View<Traits>::from_raw(image.data(), image.size());
    if (parsed.is_ok()) {
        return 1;
    }
    if (parsed.unwrap_err() == eftl::elf::ParseError::InvalidSegment) {
        return 0;
    }
    return 2;
}

auto test_rejects_invalid_load_sizes() -> int {
    auto image = make_image();
    auto segment = get<Segment>(image, sizeof(Header));
    segment.p_filesz = 2;
    segment.p_memsz = 1;
    put(image, sizeof(Header), segment);

    auto const parsed = eftl::elf::View<Traits>::from_raw(image.data(), image.size());
    if (parsed.is_ok()) {
        return 1;
    }
    if (parsed.unwrap_err() == eftl::elf::ParseError::InvalidSegment) {
        return 0;
    }
    return 2;
}

auto test_rejects_invalid_ident_and_version() -> int {
    auto invalid_class = make_image();
    invalid_class[4] = 1;
    if (eftl::elf::View<Traits>::from_raw(invalid_class.data(), invalid_class.size()).is_ok()) {
        return 1;
    }

    auto invalid_version = make_image();
    auto header = get<Header>(invalid_version, 0);
    header.e_version = 0;
    put(invalid_version, 0, header);
    if (eftl::elf::View<Traits>::from_raw(invalid_version.data(), invalid_version.size()).is_ok()) {
        return 2;
    }

    return 0;
}

auto test_legacy_policy_rejects_unmapped_entry_and_no_load() -> int {
    auto invalid_entry = make_image();
    auto header = get<Header>(invalid_entry, 0);
    header.e_entry = 0x3000;
    put(invalid_entry, 0, header);
    if (eftl::elf::View<Traits>::from_raw(invalid_entry.data(), invalid_entry.size()).is_ok()) {
        return 1;
    }

    auto no_load = make_image();
    header = get<Header>(no_load, 0);
    header.e_type = 1;
    header.e_phnum = 0;
    put(no_load, 0, header);
    if (eftl::elf::View<Traits>::from_raw(no_load.data(), no_load.size()).is_ok()) {
        return 2;
    }
    return 0;
}

auto test_legacy_view_requires_native_image_role() -> int {
    auto relocatable = make_image();
    auto header = get<Header>(relocatable, 0);
    header.e_type = 1;
    put(relocatable, 0, header);
    if (eftl::elf::View<Traits>::from_raw(relocatable.data(), relocatable.size()).is_err()) {
        return 0;
    }
    return 1;
}

auto test_legacy_policy_requires_dynamic_null() -> int {
    auto image = make_image();
    auto header = get<Header>(image, 0);
    header.e_phnum = 2;
    put(image, 0, header);

    Segment dynamic{};
    dynamic.p_type = static_cast<u32>(eftl::elf::SegType::Dynamic);
    dynamic.p_offset = sizeof(Header) + 2 * sizeof(Segment);
    dynamic.p_filesz = sizeof(Dyn);
    put(image, sizeof(Header) + sizeof(Segment), dynamic);

    Dyn entry{};
    entry.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Rel);
    put(image, dynamic.p_offset, entry);

    auto const parsed = eftl::elf::View<Traits>::from_raw(image.data(), image.size());
    if (parsed.is_ok()) {
        return 1;
    }
    if (parsed.unwrap_err() == eftl::elf::ParseError::InvalidSegment) {
        return 0;
    }
    return 2;
}
} // namespace

auto main() -> int {
    if (auto const result = test_valid_image(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_unaligned_valid_input(); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_rejects_truncated_program_table(); result != 0) {
        return 30 + result;
    }
    if (auto const result = test_rejects_invalid_load_sizes(); result != 0) {
        return 40 + result;
    }
    if (auto const result = test_rejects_invalid_ident_and_version(); result != 0) {
        return 50 + result;
    }
    if (auto const result = test_legacy_policy_rejects_unmapped_entry_and_no_load(); result != 0) {
        return 60 + result;
    }
    if (auto const result = test_legacy_policy_requires_dynamic_null(); result != 0) {
        return 70 + result;
    }
    if (auto const result = test_legacy_view_requires_native_image_role(); result != 0) {
        return 80 + result;
    }
    return 0;
}
