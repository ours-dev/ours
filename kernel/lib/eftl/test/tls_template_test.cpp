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

constexpr usize TLS_OFFSET = 0x200;
constexpr std::array<u8, 4> TLS_BYTES{0x10, 0x20, 0x30, 0x40};

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

auto tls_segment() -> Segment {
    Segment segment{};
    segment.p_type = static_cast<u32>(eftl::elf::SegType::Tls);
    segment.p_flags =
        static_cast<u32>(eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable);
    segment.p_offset = TLS_OFFSET;
    segment.p_vaddr = 0x600000;
    segment.p_filesz = TLS_BYTES.size();
    segment.p_memsz = 16;
    segment.p_align = 8;
    return segment;
}

auto make_tls_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    auto const header = make_header(1);
    put(image, 0, header);
    put(image, sizeof(Header), tls_segment());
    std::memcpy(image.data() + TLS_OFFSET, TLS_BYTES.data(), TLS_BYTES.size());
    return image;
}

auto test_observes_tls_template() -> int {
    auto const image = make_tls_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const tls = [&]() -> ustl::Option<eftl::elf::TlsTemplate<Traits>> {
        if (parsed.is_ok()) {
            return parsed->tls_template();
        }
        return ustl::NONE;
    }();
    if (tls.is_none() || tls->initialized().size() != TLS_BYTES.size() ||
        std::memcmp(tls->initialized().data(), TLS_BYTES.data(), TLS_BYTES.size()) != 0) {
        return 1;
    }
    if (tls->virtual_start() == 0x600000 && tls->memory_size() == 16 && tls->alignment() == 8 &&
        tls->permissions() == (eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable)) {
        return 0;
    }
    return 2;
}

auto test_accepts_empty_initialized_template() -> int {
    auto image = make_tls_image();
    auto segment = tls_segment();
    segment.p_offset = 0;
    segment.p_filesz = 0;
    segment.p_memsz = 32;
    segment.p_align = 16;
    put(image, sizeof(Header), segment);

    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const tls = [&]() -> ustl::Option<eftl::elf::TlsTemplate<Traits>> {
        if (parsed.is_ok()) {
            return parsed->tls_template();
        }
        return ustl::NONE;
    }();
    if (tls.is_some() && tls->initialized().empty() && tls->memory_size() == 32 &&
        tls->alignment() == 16) {
        return 0;
    }
    return 1;
}

auto test_returns_none_without_tls() -> int {
    std::array<u8, 256> image{};
    auto const header = make_header(0);
    put(image, 0, header);
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_ok() && parsed->tls_template().is_none()) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_tls_segments() -> int {
    auto duplicate = make_tls_image();
    auto const duplicate_header = make_header(2);
    put(duplicate, 0, duplicate_header);
    put(duplicate, sizeof(Header) + sizeof(Segment), tls_segment());
    auto const duplicate_result = Binary::from_raw(duplicate.data(), duplicate.size());
    if (duplicate_result.is_ok() ||
        duplicate_result.unwrap_err() != eftl::elf::ParseError::InvalidSegment) {
        return 1;
    }

    auto oversized_file = make_tls_image();
    auto oversized_file_segment = tls_segment();
    oversized_file_segment.p_memsz = TLS_BYTES.size() - 1;
    put(oversized_file, sizeof(Header), oversized_file_segment);
    auto const oversized_file_result =
        Binary::from_raw(oversized_file.data(), oversized_file.size());
    if (oversized_file_result.is_ok() ||
        oversized_file_result.unwrap_err() != eftl::elf::ParseError::InvalidSegment) {
        return 2;
    }

    auto invalid_alignment = make_tls_image();
    auto invalid_alignment_segment = tls_segment();
    invalid_alignment_segment.p_align = 3;
    put(invalid_alignment, sizeof(Header), invalid_alignment_segment);
    auto const invalid_alignment_result =
        Binary::from_raw(invalid_alignment.data(), invalid_alignment.size());
    if (invalid_alignment_result.is_ok() ||
        invalid_alignment_result.unwrap_err() != eftl::elf::ParseError::InvalidSegment) {
        return 3;
    }

    auto incongruent = make_tls_image();
    auto incongruent_segment = tls_segment();
    incongruent_segment.p_offset = TLS_OFFSET + 1;
    put(incongruent, sizeof(Header), incongruent_segment);
    auto const incongruent_result = Binary::from_raw(incongruent.data(), incongruent.size());
    if (incongruent_result.is_ok() ||
        incongruent_result.unwrap_err() != eftl::elf::ParseError::InvalidSegment) {
        return 4;
    }

    auto overflowing_virtual_end = make_tls_image();
    auto overflowing_segment = tls_segment();
    overflowing_segment.p_vaddr = std::numeric_limits<u64>::max();
    overflowing_segment.p_memsz = 1;
    overflowing_segment.p_align = 1;
    put(overflowing_virtual_end, sizeof(Header), overflowing_segment);
    auto const overflowing_result =
        Binary::from_raw(overflowing_virtual_end.data(), overflowing_virtual_end.size());
    if (overflowing_result.is_err() &&
        overflowing_result.unwrap_err() == eftl::elf::ParseError::InvalidSegment) {
        return 0;
    }
    return 5;
}

auto test_elf32_observes_tls_template() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Segment32 = Traits32::Segment;

    constexpr usize TLS32_OFFSET = 0x180;
    std::array<u8, 512> image{};
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
    segment.p_type = static_cast<u32>(eftl::elf::SegType::Tls);
    segment.p_offset = TLS32_OFFSET;
    segment.p_vaddr = 0x8048000;
    segment.p_filesz = 4;
    segment.p_memsz = 8;
    segment.p_flags =
        static_cast<u32>(eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable);
    segment.p_align = 4;
    put(image, sizeof(Header32), segment);
    std::memcpy(image.data() + TLS32_OFFSET, TLS_BYTES.data(), TLS_BYTES.size());

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    auto const tls = [&]() -> ustl::Option<eftl::elf::TlsTemplate<Traits32>> {
        if (parsed.is_ok()) {
            return parsed->tls_template();
        }
        return ustl::NONE;
    }();
    if (tls.is_some() && tls->initialized().size() == TLS_BYTES.size() &&
        tls->virtual_start() == 0x8048000 && tls->memory_size() == 8 && tls->alignment() == 4) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_tls_template(); result != 0) {
        return result;
    }
    if (auto const result = test_accepts_empty_initialized_template(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_returns_none_without_tls(); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_rejects_invalid_tls_segments(); result != 0) {
        return 30 + result;
    }
    if (test_elf32_observes_tls_template() == 0) {
        return 0;
    }
    return 40;
}
