/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/load_policy.hpp>

#include <array>

#include <cstring>

#include <ustl/collections/static_vec.hpp>

namespace {
using Traits = eftl::elf::Traits64;
using File = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Segment = Traits::Segment;

template <typename T>
using OneSegment = ustl::collections::StaticVec<T, 1>;

template <typename T>
using TwoSegments = ustl::collections::StaticVec<T, 2>;

using Policy = eftl::elf::KernelElfLoadPolicy<Traits, TwoSegments, eftl::elf::Arch::X86_64, 0x1000>;

constexpr u16 K_ET_REL = 1;
constexpr u16 K_ET_EXEC = 2;

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

auto make_header(u16 type) -> Header {
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = type;
    header.e_machine = static_cast<u16>(eftl::elf::Arch::X86_64);
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    return header;
}

template <usize SegmentCount>
auto make_image(u16 type, std::array<Segment, SegmentCount> const &segments,
                u64 entrypoint) -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    auto header = make_header(type);
    header.e_entry = entrypoint;
    header.e_phoff = sizeof(Header);
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = static_cast<u16>(SegmentCount);
    put(image, 0, header);

    for (usize index = 0; index < SegmentCount; ++index) {
        put(image, header.e_phoff + index * sizeof(Segment), segments[index]);
    }
    return image;
}

auto load_segment(u64 file_offset, u64 virtual_start, u64 file_size,
                  u64 memory_size, u64 alignment, eftl::elf::SegPerms permissions)
    -> Segment {
    Segment segment{};
    segment.p_type = static_cast<u32>(eftl::elf::SegType::Load);
    segment.p_flags = static_cast<u32>(permissions);
    segment.p_offset = file_offset;
    segment.p_vaddr = virtual_start;
    segment.p_filesz = file_size;
    segment.p_memsz = memory_size;
    segment.p_align = alignment;
    return segment;
}

auto test_makes_sparse_two_segment_executable_plan() -> int {
    auto const first =
        load_segment(0x200, 0x400200, 0x10, 0x20, 0x100,
                     eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    auto const second = load_segment(0x280, 0x402000, 0x18, 0x30, 0x8,
                                     eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable);
    auto image = make_image(K_ET_EXEC, std::array{first, second}, 0x400208);
    for (usize index = 0; index < 0x10; ++index) {
        image[0x200 + index] = static_cast<u8>(index + 1);
    }

    auto const parsed = File::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    auto const native_image = parsed->native_image();
    if (native_image.is_none()) {
        return 2;
    }
    auto const planned = Policy::make_plan(native_image.value());
    if (planned.is_err()) {
        return 3;
    }
    auto const &plan = planned.unwrap();
    auto const &segments = plan.segments();
    if (segments.size() != 2 || plan.image_base() != 0x400200 || plan.image_size() != 0x1e30 ||
        plan.entry_offset().is_none() || plan.entry_offset().value() != 8) {
        return 3;
    }
    auto const &first_plan = segments[0];
    if (first_plan.region.virtual_start != 0x400200 || first_plan.region.virtual_end != 0x400220 ||
        first_plan.region.image_offset != 0 || first_plan.content.size() != 0x10 ||
        first_plan.file_size != 0x10 || first_plan.memory_size != 0x20 ||
        first_plan.alignment != 0x100 || first_plan.region.permissions != first.permissions() ||
        first_plan.content.data()[0] != 1) {
        return 4;
    }
    auto const &second_plan = segments[1];
    if (second_plan.region.virtual_start != 0x402000 ||
        second_plan.region.virtual_end != 0x402030 || second_plan.region.image_offset != 0x1e00 ||
        second_plan.content.size() != 0x18 || second_plan.file_size != 0x18 ||
        second_plan.memory_size != 0x30 || second_plan.alignment != 8 ||
        second_plan.region.permissions != second.permissions()) {
        return 5;
    }
    return 0;
}

auto test_rejects_rel_after_structural_parse() -> int {
    auto const load = load_segment(0x100, 0x400100, 0x10, 0x10, 0x10,
                                   eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    auto const image = make_image(K_ET_REL, std::array{load}, 0x400100);
    auto const parsed = File::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->native_image().is_none()) {
        return 0;
    }
    return 2;
}

auto test_rejects_unmapped_entrypoint() -> int {
    auto const load = load_segment(0x100, 0x400100, 0x10, 0x20, 0x10,
                                   eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    auto const image = make_image(K_ET_EXEC, std::array{load}, 0x401000);
    auto const parsed = File::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->native_image().is_none()) {
        return 0;
    }
    return 2;
}

auto test_rejects_segment_capacity_excess() -> int {
    auto const first =
        load_segment(0x100, 0x400100, 0x10, 0x10, 0x10,
                     eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    auto const second = load_segment(0x200, 0x400200, 0x10, 0x10, 0x10,
                                     eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable);
    auto const image = make_image(K_ET_EXEC, std::array{first, second}, 0x400100);
    auto const parsed = File::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }

    using SmallPolicy = eftl::elf::KernelElfLoadPolicy<Traits, OneSegment, eftl::elf::Arch::X86_64>;
    auto const native_image = parsed->native_image();
    if (native_image.is_none()) {
        return 2;
    }
    auto const planned = SmallPolicy::make_plan(native_image.value());
    if (planned.is_err() && planned.unwrap_err() == eftl::elf::LoadError::CapacityExceeded) {
        return 0;
    }
    return 2;
}

auto test_rejects_wrong_target_machine() -> int {
    auto const load = load_segment(0x100, 0x400100, 0x10, 0x10, 0x10,
                                   eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    auto image = make_image(K_ET_EXEC, std::array{load}, 0x400100);
    auto header = make_header(K_ET_EXEC);
    header.e_machine = static_cast<u16>(eftl::elf::Arch::AARCH64);
    header.e_entry = 0x400100;
    header.e_phoff = sizeof(Header);
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = 1;
    put(image, 0, header);

    auto const parsed = File::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    auto const native_image = parsed->native_image();
    if (native_image.is_none()) {
        return 2;
    }
    auto const planned = Policy::make_plan(native_image.value());
    if (planned.is_err() && planned.unwrap_err() == eftl::elf::LoadError::UnsupportedMachine) {
        return 0;
    }
    return 2;
}

auto test_rejects_overlapping_memory_ranges() -> int {
    auto const first =
        load_segment(0x100, 0x400100, 0x10, 0x40, 0x10,
                     eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    auto const second = load_segment(0x200, 0x400120, 0x10, 0x40, 0x10,
                                     eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable);
    auto const image = make_image(K_ET_EXEC, std::array{first, second}, 0x400100);
    auto const parsed = File::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    if (parsed->native_image().is_none()) {
        return 0;
    }
    return 2;
}

auto test_rejects_byte_disjoint_segments_sharing_a_mapping_page() -> int {
    using PagePolicy =
        eftl::elf::KernelElfLoadPolicy<Traits, TwoSegments, eftl::elf::Arch::X86_64, 0x1000>;

    auto const first =
        load_segment(0x100, 0x400100, 0x10, 0x100, 1,
                     eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    auto const second = load_segment(0x200, 0x400200, 0x10, 0x100, 1,
                                     eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable);
    auto const image = make_image(K_ET_EXEC, std::array{first, second}, 0x400100);
    auto const parsed = File::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    auto const native_image = parsed->native_image();
    if (native_image.is_none()) {
        return 2;
    }
    auto const planned = PagePolicy::make_plan(native_image.value());
    if (planned.is_err() && planned.unwrap_err() == eftl::elf::LoadError::OverlappingPageMappings) {
        return 0;
    }
    return 2;
}

auto test_elf32_top_address_uses_u64_interval_endpoints() -> int {
    using Traits32 = eftl::elf::Traits32;
    using File32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Segment32 = Traits32::Segment;
    using Policy32 = eftl::elf::KernelElfLoadPolicy<Traits32, OneSegment, eftl::elf::Arch::I386>;

    std::array<u8, 512> image{};
    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits32::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = K_ET_EXEC;
    header.e_machine = static_cast<u16>(eftl::elf::Arch::I386);
    header.e_version = 1;
    header.e_ehsize = sizeof(Header32);
    header.e_entry = 0xfffffff0U;
    header.e_phoff = sizeof(Header32);
    header.e_phentsize = sizeof(Segment32);
    header.e_phnum = 1;
    put(image, 0, header);

    Segment32 segment{};
    segment.p_type = static_cast<u32>(eftl::elf::SegType::Load);
    segment.p_flags =
        static_cast<u32>(eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable);
    segment.p_offset = 0x100;
    segment.p_vaddr = 0xfffffff0U;
    segment.p_filesz = 1;
    segment.p_memsz = 0x10;
    segment.p_align = 1;
    put(image, header.e_phoff, segment);

    auto const parsed = File32::from_raw(image.data(), image.size());
    if (parsed.is_err()) {
        return 1;
    }
    auto const native_image = parsed->native_image();
    if (native_image.is_none()) {
        return 2;
    }
    auto const planned = Policy32::make_plan(native_image.value());
    if (planned.is_err()) {
        return 3;
    }
    auto const &plan = planned.unwrap();
    if (plan.segments().size() == 1 && plan.segments()[0].region.virtual_start == 0xfffffff0U &&
        plan.segments()[0].region.virtual_end == 0x1'0000'0000ULL && plan.image_size() == 0x10) {
        return 0;
    }
    return 3;
}
} // namespace

auto main() -> int {
    if (auto const result = test_makes_sparse_two_segment_executable_plan(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_rel_after_structural_parse(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_rejects_unmapped_entrypoint(); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_rejects_segment_capacity_excess(); result != 0) {
        return 30 + result;
    }
    if (auto const result = test_rejects_overlapping_memory_ranges(); result != 0) {
        return 40 + result;
    }
    if (auto const result = test_rejects_byte_disjoint_segments_sharing_a_mapping_page();
        result != 0) {
        return 50 + result;
    }
    if (auto const result = test_rejects_wrong_target_machine(); result != 0) {
        return 60 + result;
    }
    if (auto const result = test_elf32_top_address_uses_u64_interval_endpoints(); result != 0) {
        return 70 + result;
    }
    return 0;
}
