// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>

#include <array>
#include <limits>

#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Segment = Traits::Segment;

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

auto make_header(u16 segment_count) -> Header {
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits::ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_phoff = sizeof(Header);
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = segment_count;
    return header;
}

auto relro_segment(u64 virtual_start, u64 memory_size) -> Segment {
    Segment segment{};
    segment.p_type = static_cast<u32>(eftl::elf::SegType::GnuRelro);
    segment.p_vaddr = virtual_start;
    segment.p_memsz = memory_size;
    return segment;
}

auto make_relro_image() -> std::array<u8, 256> {
    std::array<u8, 256> image{};
    auto const header = make_header(2);
    put(image, 0, header);
    put(image, sizeof(Header), relro_segment(0x400000, 0x1200));
    put(image, sizeof(Header) + sizeof(Segment), relro_segment(0x403000, 0x400));
    return image;
}

auto test_observes_multiple_relro_regions() -> int {
    auto const image = make_relro_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    auto const regions = parsed->relro_regions();
    auto const first = regions.region_at(0);
    auto const second = regions.region_at(1);
    if (regions.size() == 2 && first.is_some() && second.is_some() &&
        first->virtual_start() == 0x400000 && first->virtual_end() == 0x401200 &&
        second->virtual_start() == 0x403000 && second->virtual_end() == 0x403400) {
        return 0;
    }
    return 2;
}

auto test_returns_empty_collection_without_relro() -> int {
    std::array<u8, 256> image{};
    auto const header = make_header(0);
    put(image, 0, header);
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    auto const regions = parsed->relro_regions();
    if (regions.size() == 0 && regions.region_at(0).is_none()) {
        return 0;
    }
    return 2;
}

auto test_rejects_overflowing_relro_end() -> int {
    auto image = make_relro_image();
    auto const header = make_header(1);
    put(image, 0, header);
    put(image, sizeof(Header), relro_segment(std::numeric_limits<u64>::max(), 1));
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err() && parsed.unwrap_err() == eftl::elf::ParseError::InvalidSegment) {
        return 0;
    }
    return 1;
}

auto test_elf32_relro_end_is_widened() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Segment32 = Traits32::Segment;

    std::array<u8, 256> image{};
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
    header.e_phnum = 1;
    put(image, 0, header);

    Segment32 segment{};
    segment.p_type = static_cast<u32>(eftl::elf::SegType::GnuRelro);
    segment.p_vaddr = 0xfffffff0U;
    segment.p_memsz = 16;
    put(image, sizeof(Header32), segment);

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    auto const regions = parsed->relro_regions();
    auto const region = regions.region_at(0);
    if (regions.size() == 1 && region.is_some() && region->virtual_start() == 0xfffffff0U &&
        region->virtual_end() == 0x1'0000'0000ULL) {
        return 0;
    }
    return 2;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_multiple_relro_regions(); result != 0) {
        return result;
    }
    if (auto const result = test_returns_empty_collection_without_relro(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_rejects_overflowing_relro_end(); result != 0) {
        return 20 + result;
    }
    if (test_elf32_relro_end_is_widened() == 0) {
        return 0;
    }
    return 30;
}
