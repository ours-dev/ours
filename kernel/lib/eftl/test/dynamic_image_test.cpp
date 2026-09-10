/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/dynamic_image.hpp>

#include <array>
#include <type_traits>

#include <cstddef>
#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Dynamic = Traits::Dyn;
using Header = Traits::Header;
using Segment = Traits::Segment;

constexpr usize PROGRAM_HEADERS_OFFSET = sizeof(Header);
constexpr usize PRIMARY_LOAD_OFFSET = 0x300;
constexpr usize DYNAMIC_OFFSET = PRIMARY_LOAD_OFFSET + 0x40;
constexpr u64 PRIMARY_LOAD_ADDRESS = 0x400000;
constexpr u64 DYNAMIC_ADDRESS = PRIMARY_LOAD_ADDRESS + 0x40;

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
auto put_segment(std::array<u8, Size> &image, usize offset, Segment const &segment, bool big_endian)
    -> void {
    put_scalar(image, offset + offsetof(Segment, p_type), segment.p_type, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_flags), segment.p_flags, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_offset), segment.p_offset, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_vaddr), segment.p_vaddr, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_paddr), segment.p_paddr, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_filesz), segment.p_filesz, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_memsz), segment.p_memsz, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_align), segment.p_align, big_endian);
}

template <usize Size>
auto put_dynamic(std::array<u8, Size> &image, usize offset, Dynamic const &dynamic, bool big_endian)
    -> void {
    put_scalar(image, offset + offsetof(Dynamic, d_tag), static_cast<u64>(dynamic.d_tag),
               big_endian);
    put_scalar(image, offset + offsetof(Dynamic, d_val), dynamic.d_val, big_endian);
}

auto make_dynamic_image(bool big_endian = false) -> std::array<u8, 2048> {
    std::array<u8, 2048> image{};
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits::ELF_CLASS;
    if (big_endian) {
        header.e_ident[5] = 2;
    } else {
        header.e_ident[5] = 1;
    }
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_phoff = PROGRAM_HEADERS_OFFSET;
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = 3;
    put_header(image, header, big_endian);

    Segment load{};
    load.p_type = static_cast<u32>(eftl::elf::SegType::Load);
    load.p_offset = PRIMARY_LOAD_OFFSET;
    load.p_vaddr = PRIMARY_LOAD_ADDRESS;
    load.p_filesz = 0x100;
    load.p_memsz = 0x120;
    load.p_align = 0x10;

    Segment dynamic{};
    dynamic.p_type = static_cast<u32>(eftl::elf::SegType::Dynamic);
    dynamic.p_offset = DYNAMIC_OFFSET;
    dynamic.p_vaddr = DYNAMIC_ADDRESS;
    dynamic.p_filesz = 2 * sizeof(Dynamic);
    dynamic.p_memsz = 2 * sizeof(Dynamic);
    dynamic.p_align = alignof(Dynamic);

    Segment second_load{};
    second_load.p_type = static_cast<u32>(eftl::elf::SegType::Load);
    second_load.p_offset = 0x500;
    second_load.p_vaddr = 0x402000;
    second_load.p_filesz = 0x20;
    second_load.p_memsz = 0x20;
    second_load.p_align = 0x10;

    put_segment(image, PROGRAM_HEADERS_OFFSET, load, big_endian);
    put_segment(image, PROGRAM_HEADERS_OFFSET + sizeof(Segment), dynamic, big_endian);
    put_segment(image, PROGRAM_HEADERS_OFFSET + 2 * sizeof(Segment), second_load, big_endian);

    Dynamic needed{};
    needed.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Needed);
    needed.d_val = 0x32;
    Dynamic terminator{};
    terminator.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::DtNull);
    put_dynamic(image, DYNAMIC_OFFSET, needed, big_endian);
    put_dynamic(image, DYNAMIC_OFFSET + sizeof(Dynamic), terminator, big_endian);
    return image;
}

auto test_observes_sectionless_dynamic_image() -> int {
    auto const bytes = make_dynamic_image();
    auto const parsed = Binary::from_raw(bytes.data(), bytes.size());
    ustl::Option<eftl::elf::DynamicImageView<Traits>> image = ustl::NONE;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    }
    ustl::Option<Dynamic> entry = ustl::NONE;
    if (image.is_some()) {
        entry = image->entry_at(0);
    }
    eftl::elf::TableView<Dynamic> entries{};
    if (image.is_some()) {
        entries = image->entries();
    }
    ustl::Option<eftl::ByteRange> mapped = ustl::NONE;
    if (image.is_some()) {
        mapped = image->bytes_at_virtual_address(DYNAMIC_ADDRESS, sizeof(Dynamic));
    }
    if (image.is_some() && image->size() == 1 && entries.size() == 2 && entry.is_some() &&
        eftl::elf::DynamicImageView<Traits>::tag(entry.value()) == eftl::elf::DynTag::Needed &&
        eftl::elf::DynamicImageView<Traits>::value(entry.value()) == 0x32 && mapped.is_some() &&
        mapped->data() == bytes.data() + DYNAMIC_OFFSET && mapped->size() == sizeof(Dynamic) &&
        image->bytes_at_virtual_address(PRIMARY_LOAD_ADDRESS + 0x100, 1).is_none() &&
        image->bytes_at_virtual_address(PRIMARY_LOAD_ADDRESS + 0xf8, 0x100).is_none()) {
        return 0;
    }
    return 1;
}

auto test_observes_big_endian_sectionless_dynamic_image() -> int {
    auto const bytes = make_dynamic_image(true);
    auto const parsed = Binary::from_raw(bytes.data(), bytes.size());
    ustl::Option<eftl::elf::DynamicImageView<Traits>> image = ustl::NONE;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    }
    ustl::Option<Dynamic> entry = ustl::NONE;
    if (image.is_some()) {
        entry = image->entry_at(0);
    }
    if (image.is_some() && image->size() == 1 && entry.is_some() &&
        eftl::elf::DynamicImageView<Traits>::tag(entry.value()) == eftl::elf::DynTag::Needed &&
        eftl::elf::DynamicImageView<Traits>::value(entry.value()) == 0x32) {
        return 0;
    }
    return 1;
}

auto test_observes_elf32_sectionless_dynamic_image() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Dynamic32 = Traits32::Dyn;
    using Header32 = Traits32::Header;
    using Segment32 = Traits32::Segment;

    std::array<u8, 512> bytes{};
    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits32::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header32);
    header.e_phoff = sizeof(Header32);
    header.e_phentsize = sizeof(Segment32);
    header.e_phnum = 2;
    std::memcpy(bytes.data(), &header, sizeof(header));

    Segment32 load{};
    load.p_type = static_cast<u32>(eftl::elf::SegType::Load);
    load.p_offset = 0x100;
    load.p_vaddr = 0x4000;
    load.p_filesz = 0x30;
    load.p_memsz = 0x30;
    load.p_align = 4;
    std::memcpy(bytes.data() + sizeof(Header32), &load, sizeof(load));
    Segment32 dynamic{};
    dynamic.p_type = static_cast<u32>(eftl::elf::SegType::Dynamic);
    dynamic.p_offset = 0x110;
    dynamic.p_vaddr = 0x4010;
    dynamic.p_filesz = 2 * sizeof(Dynamic32);
    dynamic.p_memsz = 2 * sizeof(Dynamic32);
    dynamic.p_align = alignof(Dynamic32);
    std::memcpy(bytes.data() + sizeof(Header32) + sizeof(Segment32), &dynamic, sizeof(dynamic));

    Dynamic32 flags{};
    flags.d_tag = static_cast<Traits32::Sword>(eftl::elf::DynTag::Flags);
    flags.d_val = 0x12345678U;
    Dynamic32 terminator{};
    std::memcpy(bytes.data() + dynamic.p_offset, &flags, sizeof(flags));
    std::memcpy(bytes.data() + dynamic.p_offset + sizeof(flags), &terminator, sizeof(terminator));

    auto const parsed = Binary32::from_raw(bytes.data(), bytes.size());
    ustl::Option<eftl::elf::DynamicImageView<Traits32>> image = ustl::NONE;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    }
    ustl::Option<Dynamic32> entry = ustl::NONE;
    if (image.is_some()) {
        entry = image->entry_at(0);
    }
    if (image.is_some() && entry.is_some() && image->size() == 1 &&
        eftl::elf::DynamicImageView<Traits32>::tag(entry.value()) == eftl::elf::DynTag::Flags &&
        eftl::elf::DynamicImageView<Traits32>::value(entry.value()) == 0x12345678U) {
        return 0;
    }
    return 1;
}

auto test_rejects_malformed_dynamic_images() -> int {
    auto duplicate = make_dynamic_image();
    Header header{};
    std::memcpy(&header, duplicate.data(), sizeof(header));
    header.e_phnum = 4;
    std::memcpy(duplicate.data(), &header, sizeof(header));
    Segment dynamic{};
    std::memcpy(&dynamic, duplicate.data() + PROGRAM_HEADERS_OFFSET + sizeof(Segment),
                sizeof(dynamic));
    std::memcpy(duplicate.data() + PROGRAM_HEADERS_OFFSET + 3 * sizeof(Segment), &dynamic,
                sizeof(dynamic));
    auto const duplicate_parsed = Binary::from_raw(duplicate.data(), duplicate.size());
    if (duplicate_parsed.is_err() || duplicate_parsed->dynamic_image().is_some()) {
        return 1;
    }

    auto missing_terminator = make_dynamic_image();
    Dynamic nonnull{};
    nonnull.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Needed);
    nonnull.d_val = 0x33;
    put_dynamic(missing_terminator, DYNAMIC_OFFSET + sizeof(Dynamic), nonnull, false);
    auto const missing_terminator_parsed =
        Binary::from_raw(missing_terminator.data(), missing_terminator.size());
    if (missing_terminator_parsed.is_err() ||
        missing_terminator_parsed->dynamic_image().is_some()) {
        return 2;
    }

    auto zero_fill_dynamic = make_dynamic_image();
    std::memcpy(&dynamic, zero_fill_dynamic.data() + PROGRAM_HEADERS_OFFSET + sizeof(Segment),
                sizeof(dynamic));
    dynamic.p_vaddr = PRIMARY_LOAD_ADDRESS + 0x100;
    std::memcpy(zero_fill_dynamic.data() + PROGRAM_HEADERS_OFFSET + sizeof(Segment), &dynamic,
                sizeof(dynamic));
    auto const zero_fill_parsed =
        Binary::from_raw(zero_fill_dynamic.data(), zero_fill_dynamic.size());
    if (zero_fill_parsed.is_err() || zero_fill_parsed->dynamic_image().is_some()) {
        return 3;
    }

    auto no_dynamic = make_dynamic_image();
    std::memcpy(&dynamic, no_dynamic.data() + PROGRAM_HEADERS_OFFSET + sizeof(Segment),
                sizeof(dynamic));
    dynamic.p_type = static_cast<u32>(eftl::elf::SegType::Note);
    std::memcpy(no_dynamic.data() + PROGRAM_HEADERS_OFFSET + sizeof(Segment), &dynamic,
                sizeof(dynamic));
    auto const no_dynamic_parsed = Binary::from_raw(no_dynamic.data(), no_dynamic.size());
    if (no_dynamic_parsed.is_err() || no_dynamic_parsed->dynamic_image().is_some()) {
        return 4;
    }

    auto ambiguous_mapping = make_dynamic_image();
    std::memcpy(&header, ambiguous_mapping.data(), sizeof(header));
    header.e_phnum = 4;
    std::memcpy(ambiguous_mapping.data(), &header, sizeof(header));
    Segment load{};
    std::memcpy(&load, ambiguous_mapping.data() + PROGRAM_HEADERS_OFFSET, sizeof(load));
    std::memcpy(ambiguous_mapping.data() + PROGRAM_HEADERS_OFFSET + 3 * sizeof(Segment), &load,
                sizeof(load));
    auto const ambiguous_parsed =
        Binary::from_raw(ambiguous_mapping.data(), ambiguous_mapping.size());
    if (ambiguous_parsed.is_ok() && ambiguous_parsed->dynamic_image().is_none()) {
        return 0;
    }
    return 5;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_sectionless_dynamic_image(); result != 0) {
        return result;
    }
    if (auto const result = test_observes_big_endian_sectionless_dynamic_image(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_observes_elf32_sectionless_dynamic_image(); result != 0) {
        return 20 + result;
    }
    if (test_rejects_malformed_dynamic_images() == 0) {
        return 0;
    }
    return 30;
}
