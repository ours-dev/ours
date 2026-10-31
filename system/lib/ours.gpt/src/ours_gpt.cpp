/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours_gpt/ours_gpt.hpp>

#include <algorithm>
#include <array>

namespace ours_gpt {
namespace {
    // The table of the reflected polynomial 0xEDB88320, the one the CRC-32 the
    // specification names runs on, computed once at compile time: bit b of the
    // byte is the coefficient of x^(7-b), so each entry is the CRC of one byte.
    consteval auto crc_table() -> std::array<u32, 256> {
        auto table = std::array<u32, 256>{};
        for (auto index = usize{0}; index < table.size(); ++index) {
            auto value = static_cast<u32>(index);
            for (auto bit = usize{0}; bit < 8; ++bit) {
                value = (value & 1) != 0 ? (value >> 1) ^ 0xEDB8'8320 : value >> 1;
            }
            table[index] = value;
        }
        return table;
    }

    constexpr auto TABLE = crc_table();

    auto le16(std::span<u8 const> bytes, usize const offset) -> u16 {
        return static_cast<u16>(bytes[offset]) |
               static_cast<u16>(static_cast<u16>(bytes[offset + 1]) << 8);
    }

    auto le32(std::span<u8 const> bytes, usize const offset) -> u32 {
        return static_cast<u32>(bytes[offset]) | (static_cast<u32>(bytes[offset + 1]) << 8) |
               (static_cast<u32>(bytes[offset + 2]) << 16) |
               (static_cast<u32>(bytes[offset + 3]) << 24);
    }

    auto le64(std::span<u8 const> bytes, usize const offset) -> u64 {
        return static_cast<u64>(le32(bytes, offset)) |
               (static_cast<u64>(le32(bytes, offset + 4)) << 32);
    }
} // namespace

auto crc32(std::span<u8 const> bytes, u32 const seed) -> u32 {
    // Inverted at both ends, so a call seeded with a previous answer chains:
    // crc32(b, crc32(a)) is crc32(a + b).
    auto crc = ~seed;
    for (auto const byte: bytes) {
        crc = TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

auto parse_header(std::span<u8 const> block) -> ustl::Result<Header, ours::Status> {
    if (block.size() < HEADER_CRC_BYTES) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto const signature = std::span{block}.first(SIGNATURE.size());
    auto const is_gpt =
        std::equal(signature.begin(), signature.end(), SIGNATURE.begin(), SIGNATURE.end());
    if (!is_gpt) {
        return ustl::err(ours::Status::NotFound);
    }

    // Revision 1.0 is the one there is; a header its size cannot hold, or a
    // revision this parser does not know, is a table it cannot vouch for.
    if (le32(block, 8) != 0x0001'0000) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto const header_bytes = le32(block, 12);
    if (header_bytes < HEADER_CRC_BYTES) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto const recorded = le32(block, 16);
    auto zeroed = std::array<u8, HEADER_CRC_BYTES>{};
    std::copy_n(block.begin(), zeroed.size(), zeroed.begin());
    for (auto index = usize{0}; index < 4; ++index) {
        zeroed[16 + index] = 0;
    }
    if (crc32(zeroed) != recorded) {
        return ustl::err(ours::Status::BadState);
    }

    auto const header = Header{
        .entries_lba = le64(block, 72),
        .num_entries = le32(block, 80),
        .entry_bytes = le32(block, 84),
    };
    if (header.entries_lba == 0 || header.num_entries == 0 || header.num_entries > MAX_ENTRIES ||
        header.entry_bytes < MIN_ENTRY_BYTES) {
        return ustl::err(ours::Status::InvArgs);
    }
    // The array is read and parsed one block at a time, which is only sound
    // while no entry straddles two blocks.
    if (512 % header.entry_bytes != 0) {
        return ustl::err(ours::Status::InvArgs);
    }
    return ustl::ok(header);
}

auto parse_entries(std::span<u8 const> bytes, usize const first, Header const &header,
                   std::span<Partition> out) -> ustl::Result<usize, ours::Status> {
    if (bytes.size() % header.entry_bytes != 0) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto const total = bytes.size() / header.entry_bytes;
    if (first + total > out.size()) {
        return ustl::err(ours::Status::BufferTooSmall);
    }
    auto written = usize{0};
    for (auto index = usize{0}; index < total; ++index) {
        auto const entry = bytes.subspan(index * header.entry_bytes);
        // An entry whose type GUID is all zero is the free slot it starts
        // as: nothing there, nothing to report.
        auto const type = std::span{entry}.first(16);
        auto const free_slot =
            std::all_of(type.begin(), type.end(), [](u8 const byte) { return byte == 0; });
        if (free_slot) {
            continue;
        }
        auto &partition = out[first + written];
        std::copy_n(type.begin(), partition.type.bytes.size(), partition.type.bytes.begin());
        std::copy_n(entry.begin() + 16, partition.id.bytes.size(), partition.id.bytes.begin());
        partition.first_lba = le64(entry, 32);
        partition.last_lba = le64(entry, 40);
        partition.attributes = le64(entry, 48);
        // The name is UTF-16LE; the units below 0x80 are ASCII and the rest
        // decode to '?', which keeps the check's lines honest without
        // pulling in a conversion table.
        for (auto unit = usize{0}; unit < NAME_UNITS; ++unit) {
            auto const code = le16(entry, 56 + unit * 2);
            partition.name[unit] = code < 0x80 ? static_cast<char>(code) : '?';
        }
        partition.name[NAME_UNITS] = '\0';
        written += 1;
    }
    return ustl::ok(written);
}
} // namespace ours_gpt
