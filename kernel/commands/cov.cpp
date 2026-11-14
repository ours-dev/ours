/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `cov dump`: print the kernel's source-based coverage counters as a raw
// profile, framed as text lines for scripts/cov_kernel.py to turn back into a
// file llvm-profdata reads. The profile itself is built by //system/lib/profdata;
// this file is only the console's side of it.
//
// The console is shared. The debug image's other CPUs log thousands of lines a
// second, and each line this command writes lands whole between theirs, but
// never in a predictable place. So every line says what it is: a `COV` line
// carries its own offset into the profile, the script keeps only lines with
// the tag, and the closing line carries a checksum of all the bytes, so a line
// lost or damaged shows up as a mismatch and the script asks again.
#include <ours/mm/alloc_flags.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <string_view>

#include <cstddef>
#include <cstring>

#include <gktl/command.hpp>
#include <gktl/options.hpp>
#include <gktl/report.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <profdata/profdata.hpp>
#include <ustl/fmt/format_to.hpp>

// NOLINTBEGIN(readability-identifier-naming): linker-defined symbols, named by
// kernel.ld.
extern "C" {
extern char const __build_id_note_start[];
extern char const __build_id_note_end[];
}
// NOLINTEND(readability-identifier-naming)

namespace {

/// A reply to whoever typed the command, written straight to the console sink
/// as one line and not through the log: it is not something the kernel said,
/// and a log floor must not be able to silence it.
template <typename... Args>
auto reply(std::format_string<Args const &...> fmt, Args const &...args) -> void {
    char line[128];
    auto const result = ustl::fmt::format_to_n(line, sizeof(line) - 1, fmt, args...);
    auto length = static_cast<usize>(result.out - line);
    line[length++] = '\n';
    logz4::get_global_logger().write(line, length);
}

/// The build ID the linker put in the kernel image's note: a header of the name's
/// size, the ID's size and a type, 4 bytes each; the name, padded to 4 bytes;
/// then the ID. Empty if the note is not what that describes.
auto build_id() -> std::span<std::byte const> {
    constexpr usize NOTE_HEADER_SIZE = 12;
    constexpr u32 NAME_ALIGN = 4;

    auto const *const note = reinterpret_cast<std::byte const *>(__build_id_note_start);
    auto const note_size = static_cast<usize>(__build_id_note_end - __build_id_note_start);
    if (note_size < NOTE_HEADER_SIZE) {
        return {};
    }
    u32 name_size = 0;
    u32 id_size = 0;
    std::memcpy(&name_size, note, sizeof(name_size));
    std::memcpy(&id_size, note + sizeof(name_size), sizeof(id_size));
    usize const id_offset = NOTE_HEADER_SIZE + ((name_size + (NAME_ALIGN - 1)) & ~(NAME_ALIGN - 1));
    if (id_offset + id_size > note_size) {
        return {};
    }
    return {note + id_offset, id_size};
}

/// Base64 of `bytes`, with padding, written to `out`, which must have room for
/// four characters per three bytes rounded up. Returns the length written.
auto encode_base64(std::span<std::byte const> const bytes, char *const out) -> usize {
    constexpr std::string_view ALPHABET =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    usize length = 0;
    for (usize i = 0; i < bytes.size(); i += 3) {
        auto const remaining = bytes.size() - i;
        u32 group = static_cast<u32>(bytes[i]) << 16;
        if (remaining > 1) {
            group |= static_cast<u32>(bytes[i + 1]) << 8;
        }
        if (remaining > 2) {
            group |= static_cast<u32>(bytes[i + 2]);
        }
        out[length++] = ALPHABET[(group >> 18) & 0x3f];
        out[length++] = ALPHABET[(group >> 12) & 0x3f];
        if (remaining > 1) {
            out[length++] = ALPHABET[(group >> 6) & 0x3f];
        } else {
            out[length++] = '=';
        }
        if (remaining > 2) {
            out[length++] = ALPHABET[group & 0x3f];
        } else {
            out[length++] = '=';
        }
    }
    return length;
}

/// FNV-1a over `bytes`: cheap, and enough to tell a profile that arrived whole
/// from one that did not.
auto checksum(std::span<std::byte const> const bytes) -> u64 {
    constexpr u64 OFFSET_BASIS = 0xcbf29ce484222325;
    constexpr u64 PRIME = 0x100000001b3;

    u64 hash = OFFSET_BASIS;
    for (auto const byte: bytes) {
        hash ^= static_cast<u64>(byte);
        hash *= PRIME;
    }
    return hash;
}

/// Bytes of profile per line. 144 is divisible by three, so every line's base64
/// is whole, and its 192 characters leave room for the tag and the offset.
constexpr usize CHUNK_BYTES = 144;

/// The longest line: the tag, an offset, a space, the base64 of a chunk, a
/// newline.
constexpr usize LINE_SIZE = 4 + 20 + 1 + ((CHUNK_BYTES / 3) * 4) + 1;

auto print_profile(std::span<std::byte const> const profile) -> void {
    reply("COV-BEGIN {} {}", profile.size(), CHUNK_BYTES);

    char line[LINE_SIZE];
    for (usize offset = 0; offset < profile.size(); offset += CHUNK_BYTES) {
        auto const chunk = profile.subspan(offset, std::min(CHUNK_BYTES, profile.size() - offset));
        auto const prefix = ustl::fmt::format_to_n(line, 4 + 20 + 1, "COV {} ", offset);
        auto *out = prefix.out;
        out += encode_base64(chunk, out);
        *out++ = '\n';
        logz4::get_global_logger().write(line, static_cast<usize>(out - line));
    }

    reply("COV-END {} {:016x}", profile.size(), checksum(profile));
}

/// `cov dump`
auto cov_dump() -> int {
    profdata::RawProfile const profile{build_id()};
    if (profile.size_bytes() == 0) {
        reply("cov: this kernel was built without coverage instrumentation");
        return 1;
    }

    // 64-bit words, because the counters in the profile are and the buffer must
    // start on a multiple of profdata::RawProfile::ALIGNMENT.
    auto const word_count = (profile.size_bytes() + sizeof(u64) - 1) / sizeof(u64);
    auto *const words = new (ours::mm::ALLOC_KERNEL) u64[word_count]();
    if (words == nullptr) {
        reply("cov: no memory for a {}-byte profile", profile.size_bytes());
        return 1;
    }

    std::span<std::byte> const dump{reinterpret_cast<std::byte *>(words), profile.size_bytes()};
    profile.copy_live(profile.write_fixed(dump));
    print_profile(dump);

    delete[] words;
    return 0;
}

auto cov_command(gktl::Invocation const &invocation) -> int {
    constexpr std::string_view VERBS[] = {"dump"};
    auto options = gktl::Options{"cov"};
    options.add<std::string_view>("verb").required().choices(VERBS);

    auto const parsed = options.parse(invocation);
    if (parsed.is_err()) {
        gktl::reject(options, parsed.unwrap_err());
        return 1;
    }
    return cov_dump();
}

} // namespace

GKTL_COMMAND(CovDump, "cov", cov_command,
             "cov verb=dump: print the kernel's coverage counters as a raw profile, for "
             "scripts/cov_kernel.py to turn into a file llvm-profdata reads")
