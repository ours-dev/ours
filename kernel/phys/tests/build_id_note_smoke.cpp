/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <array>
#include <string_view>

#include <cstdlib>

#include "../build_id_note.hpp"

namespace {
using ours::phys::hex_encode_build_id;

auto require(bool const condition) -> void {
    if (!condition) {
        std::abort();
    }
}

// A real .note.gnu.build-id section (36 bytes: 12-byte header + "GNU\0" +
// 20-byte sha1 descriptor, no trailing padding -- 4-byte-aligned, the
// universal GNU convention), extracted from an actual linked physload
// binary with `llvm-objcopy --dump-section=.note.gnu.build-id=... physload
// /dev/null` and cross-checked against `llvm-readelf --notes`'s report of
// "Build ID: df847a5623d4ecaf766e8cb939ef8fd54ff9cb70".  This is what makes
// the test worth having: hand-built fixtures tend to reproduce whatever
// alignment assumption the code under test already makes, which is exactly
// how the same mistake this function works around (see the eftl
// NoteSectionView finding noted in kernel/lib/eftl) went unnoticed by its
// own test suite.
constexpr std::array<u8, 36> REAL_BUILD_ID_NOTE = {
    0x04, 0x00, 0x00, 0x00,                                     // n_namesz = 4
    0x14, 0x00, 0x00, 0x00,                                     // n_descsz = 20
    0x03, 0x00, 0x00, 0x00,                                     // n_type   = NT_GNU_BUILD_ID
    'G',  'N',  'U',  0x00,                                     // name = "GNU\0"
    0xdf, 0x84, 0x7a, 0x56, 0x23, 0xd4, 0xec, 0xaf, 0x76, 0x6e, // descriptor...
    0x8c, 0xb9, 0x39, 0xef, 0x8f, 0xd5, 0x4f, 0xf9, 0xcb, 0x70, // ...20 bytes
};
constexpr std::string_view EXPECTED_HEX = "df847a5623d4ecaf766e8cb939ef8fd54ff9cb70";

auto test_real_note() -> void {
    char buffer[64];
    auto const length = hex_encode_build_id(REAL_BUILD_ID_NOTE, buffer);
    require(length == EXPECTED_HEX.size());
    require(std::string_view(buffer, length) == EXPECTED_HEX);
    require(buffer[length] == '\0');
}

auto test_truncated_header() -> void {
    char buffer[64];
    require(hex_encode_build_id(std::span(REAL_BUILD_ID_NOTE).subspan(0, 8), buffer) == 0);
}

auto test_wrong_owner_name() -> void {
    auto note = REAL_BUILD_ID_NOTE;
    note[12] = 'X'; // "GNU" -> "XNU"
    char buffer[64];
    require(hex_encode_build_id(note, buffer) == 0);
}

auto test_wrong_type() -> void {
    auto note = REAL_BUILD_ID_NOTE;
    note[8] = 0x09; // type 3 -> 9 (not NT_GNU_BUILD_ID)
    char buffer[64];
    require(hex_encode_build_id(note, buffer) == 0);
}

auto test_second_note_appended() -> void {
    // A second (any) note tacked on after the real one must be rejected,
    // not silently ignored -- see the "exactly one note" comment in
    // build-id-note.cpp.
    std::array<u8, 48> two_notes{};
    std::copy(REAL_BUILD_ID_NOTE.begin(), REAL_BUILD_ID_NOTE.end(), two_notes.begin());
    // A minimal well-formed (but truncated/garbage) trailing note header is
    // enough; hex_encode_build_id must never get past the "exactly one
    // note" check to look at it.
    char buffer[64];
    require(hex_encode_build_id(two_notes, buffer) == 0);
}

auto test_output_buffer_too_small() -> void {
    char buffer[8]; // needs 41 (40 hex chars + NUL)
    require(hex_encode_build_id(REAL_BUILD_ID_NOTE, buffer) == 0);
}
} // namespace

auto main() -> int {
    test_real_note();
    test_truncated_header();
    test_wrong_owner_name();
    test_wrong_type();
    test_second_note_appended();
    test_output_buffer_too_small();
    return 0;
}
