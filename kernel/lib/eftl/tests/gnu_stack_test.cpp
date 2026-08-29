// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>

#include <array>

#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Segment = Traits::Segment;

constexpr u64 STACK_SIZE = 0x24000;

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

auto gnu_stack_segment() -> Segment {
    Segment segment{};
    segment.p_type = static_cast<u32>(eftl::elf::SegType::GnuStack);
    segment.p_flags =
        static_cast<u32>(eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable);
    segment.p_memsz = STACK_SIZE;
    return segment;
}

auto make_gnu_stack_image() -> std::array<u8, 256> {
    std::array<u8, 256> image{};
    auto const header = make_header(1);
    put(image, 0, header);
    put(image, sizeof(Header), gnu_stack_segment());
    return image;
}

auto test_observes_gnu_stack() -> int {
    auto const image = make_gnu_stack_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const stack = [&]() -> ustl::Option<eftl::elf::GnuStack<Traits>> {
        if (parsed.is_ok()) {
            return parsed->gnu_stack();
        }
        return ustl::NONE;
    }();
    if (stack.is_some() && stack->size_hint() == STACK_SIZE &&
        stack->permissions() == (eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable)) {
        return 0;
    }
    return 1;
}

auto test_returns_none_without_gnu_stack() -> int {
    std::array<u8, 256> image{};
    auto const header = make_header(0);
    put(image, 0, header);
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_ok() && parsed->gnu_stack().is_none()) {
        return 0;
    }
    return 1;
}

auto test_rejects_duplicate_gnu_stack() -> int {
    auto image = make_gnu_stack_image();
    auto const header = make_header(2);
    put(image, 0, header);
    put(image, sizeof(Header) + sizeof(Segment), gnu_stack_segment());
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_err() && parsed.unwrap_err() == eftl::elf::ParseError::InvalidSegment) {
        return 0;
    }
    return 1;
}

auto test_elf32_observes_gnu_stack() -> int {
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
    segment.p_type = static_cast<u32>(eftl::elf::SegType::GnuStack);
    segment.p_flags = static_cast<u32>(eftl::elf::SegPerms::Readable);
    segment.p_memsz = 0x10000;
    put(image, sizeof(Header32), segment);

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    auto const stack = [&]() -> ustl::Option<eftl::elf::GnuStack<Traits32>> {
        if (parsed.is_ok()) {
            return parsed->gnu_stack();
        }
        return ustl::NONE;
    }();
    if (stack.is_some() && stack->size_hint() == 0x10000 &&
        stack->permissions() == eftl::elf::SegPerms::Readable) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_gnu_stack(); result != 0) {
        return result;
    }
    if (auto const result = test_returns_none_without_gnu_stack(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_rejects_duplicate_gnu_stack(); result != 0) {
        return 20 + result;
    }
    if (test_elf32_observes_gnu_stack() == 0) {
        return 0;
    }
    return 30;
}
