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

constexpr usize K_INTERPRETER_OFFSET = 0x200;
constexpr usize K_LOAD_OFFSET = 0x300;
constexpr std::array<u8, 16> K_INTERPRETER_PATH{
    '/', 'l', 'i', 'b', '/', 'l', 'd', '-', 'o', 'u', 'r', 's', '.', 's', 'o', 0,
};

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

auto make_header(u16 segment_count) -> Header {
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits::K_ELF_CLASS;
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

auto interpreter_segment() -> Segment {
    Segment segment{};
    segment.p_type = static_cast<ours::u32>(eftl::elf::SegType::Interp);
    segment.p_offset = K_INTERPRETER_OFFSET;
    segment.p_filesz = K_INTERPRETER_PATH.size();
    return segment;
}

auto load_segment() -> Segment {
    Segment segment{};
    segment.p_type = static_cast<ours::u32>(eftl::elf::SegType::Load);
    segment.p_offset = K_LOAD_OFFSET;
    segment.p_vaddr = 0x400000;
    segment.p_filesz = 16;
    segment.p_memsz = 16;
    segment.p_align = 1;
    return segment;
}

auto make_interpreter_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    auto const header = make_header(2);
    put(image, 0, header);
    put(image, sizeof(Header), interpreter_segment());
    put(image, sizeof(Header) + sizeof(Segment), load_segment());
    std::memcpy(image.data() + K_INTERPRETER_OFFSET, K_INTERPRETER_PATH.data(),
                K_INTERPRETER_PATH.size());
    return image;
}

auto equals(eftl::ByteRange range, char const *text, usize size) -> bool {
    return range.size() == size && std::memcmp(range.data(), text, size) == 0;
}

auto test_observes_interpreter_path() -> int {
    auto const image = make_interpreter_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    auto const path = [&]() -> ustl::Option<eftl::ByteRange> {
        if (parsed.is_ok()) {
            return parsed->interpreter_path();
        }
        return ustl::NONE;
    }();
    if (path.is_some() && equals(path.value(), "/lib/ld-ours.so", 15)) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_interpreter_segments() -> int {
    auto duplicate = make_interpreter_image();
    put(duplicate, sizeof(Header) + sizeof(Segment), interpreter_segment());
    auto const duplicate_result = Binary::from_raw(duplicate.data(), duplicate.size());
    if (duplicate_result.is_ok() ||
        duplicate_result.unwrap_err() != eftl::elf::ParseError::InvalidSegment) {
        return 1;
    }

    auto after_load = make_interpreter_image();
    put(after_load, sizeof(Header), load_segment());
    put(after_load, sizeof(Header) + sizeof(Segment), interpreter_segment());
    auto const after_load_result = Binary::from_raw(after_load.data(), after_load.size());
    if (after_load_result.is_ok() ||
        after_load_result.unwrap_err() != eftl::elf::ParseError::InvalidSegment) {
        return 2;
    }

    auto unterminated = make_interpreter_image();
    unterminated[K_INTERPRETER_OFFSET + K_INTERPRETER_PATH.size() - 1] = 'x';
    auto const unterminated_result = Binary::from_raw(unterminated.data(), unterminated.size());
    if (unterminated_result.is_ok() ||
        unterminated_result.unwrap_err() != eftl::elf::ParseError::InvalidSegment) {
        return 3;
    }

    auto embedded_nul = make_interpreter_image();
    embedded_nul[K_INTERPRETER_OFFSET + 4] = 0;
    auto const embedded_nul_result = Binary::from_raw(embedded_nul.data(), embedded_nul.size());
    if (embedded_nul_result.is_ok() ||
        embedded_nul_result.unwrap_err() != eftl::elf::ParseError::InvalidSegment) {
        return 4;
    }

    auto empty = make_interpreter_image();
    auto segment = interpreter_segment();
    segment.p_filesz = 0;
    put(empty, sizeof(Header), segment);
    auto const empty_result = Binary::from_raw(empty.data(), empty.size());
    if (empty_result.is_err() &&
        empty_result.unwrap_err() == eftl::elf::ParseError::InvalidSegment) {
        return 0;
    }
    return 5;
}

auto test_returns_none_without_an_interpreter() -> int {
    std::array<u8, 256> image{};
    auto const header = make_header(0);
    put(image, 0, header);
    auto const parsed = Binary::from_raw(image.data(), image.size());
    if (parsed.is_ok() && parsed->interpreter_path().is_none()) {
        return 0;
    }
    return 1;
}

auto test_elf32_observes_interpreter_path() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Segment32 = Traits32::Segment;

    constexpr usize K_PATH_OFFSET = 0x140;
    std::array<u8, 512> image{};
    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits32::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header32);
    header.e_phoff = sizeof(Header32);
    header.e_phentsize = sizeof(Segment32);
    header.e_phnum = 1;
    put(image, 0, header);
    Segment32 interpreter{};
    interpreter.p_type = static_cast<ours::u32>(eftl::elf::SegType::Interp);
    interpreter.p_offset = K_PATH_OFFSET;
    interpreter.p_filesz = 9;
    put(image, sizeof(Header32), interpreter);
    std::array<u8, 9> const path{'/', 'l', 'i', 'b', '/', 'l', 'd', '3', 0};
    std::memcpy(image.data() + K_PATH_OFFSET, path.data(), path.size());

    auto const parsed = Binary32::from_raw(image.data(), image.size());
    auto const observed = [&]() -> ustl::Option<eftl::ByteRange> {
        if (parsed.is_ok()) {
            return parsed->interpreter_path();
        }
        return ustl::NONE;
    }();
    if (observed.is_some() && equals(observed.value(), "/lib/ld3", 8)) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_interpreter_path(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_invalid_interpreter_segments(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_returns_none_without_an_interpreter(); result != 0) {
        return 20 + result;
    }
    if (test_elf32_observes_interpreter_path() == 0) {
        return 0;
    }
    return 30;
}
