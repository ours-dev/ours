/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_GPT_OURS_GPT_HPP
#define OURS_GPT_OURS_GPT_HPP 1

#include <array>
#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

/// GPT, the partition table a UEFI disk carries, read over a block device.
///
/// The objects the disk holds, all in its first LBAs:
///   - the protective MBR, LBA 0: a legacy partition table whose one entry
///     claims the whole disk, so a program that reads only MBRs leaves the
///     GPT alone. This library checks its boot signature and nothing more;
///   - the GPT header, LBA 1: the signature, the CRC32 of itself, where the
///     partition entries live and how many there are;
///   - the partition entries: type GUID, the partition's own GUID, the first
///     and last LBA it covers, attributes, and a name.
///
/// The events of reading one: read LBA 0, check the boot signature; read LBA
/// 1, check the signature and the header's own CRC32; read the entry array
/// block by block, accumulating the CRC32 the header records for it. Any
/// check that fails refuses the whole table -- a CRC covers the bytes that
/// name the partitions, so a mismatch means some of those bytes are not the
/// ones the disk was written with, and which of them it is is not knowable.
///
/// All fields on disk are little-endian; a GUID's first three fields are the
/// little-endian encoding of their textual form. The parser keeps the sixteen
/// bytes as they lie and compares them as bytes.
namespace ours_gpt {
/// The signature the header starts with, "EFI PART".
inline constexpr auto SIGNATURE = std::string_view{"EFI PART"};

/// How much of the header the self-CRC covers, with the CRC field zeroed.
inline constexpr auto HEADER_CRC_BYTES = usize{92};

/// The smallest entry the specification allows, and the most entries this
/// library reads: a hostile header's array is capped, since it is read block
/// by block into the caller's buffers.
inline constexpr auto MIN_ENTRY_BYTES = usize{128};
inline constexpr auto MAX_ENTRIES = usize{128};

/// The name's UTF-16LE code units, and the ASCII decode of one plus its NUL.
inline constexpr auto NAME_UNITS = usize{36};

/// The type GUID the disks this project writes carry: "ours filesystem",
/// textually 5E3C4A9D-2B71-4F08-9A3D-6CE1F042B867, whose on-disk bytes are
/// the mixed-endian order the specification lays a GUID down in. A partition
/// of any other type parses all the same; this is the one an ours filesystem
/// is expected to sit in, and scripts/make_gpt_disk.py writes it.
inline constexpr auto OURS_FS_TYPE_BYTES = std::array<u8, 16>{
    0x9d, 0x4a, 0x3c, 0x5e, 0x71, 0x2b, 0x08, 0x4f, 0x9a, 0x3d, 0x6c, 0xe1, 0xf0, 0x42, 0xb8, 0x67};

/// A GUID, the sixteen bytes as they lie on disk.
struct Guid {
    std::array<u8, 16> bytes = {};

    [[nodiscard]]
    auto operator==(Guid const &) const -> bool = default;
};

/// One used partition entry. |last_lba| is the last block the partition
/// covers, inclusive, as the specification names it.
struct Partition {
    Guid type;
    Guid id;
    u64 first_lba = 0;
    u64 last_lba = 0;
    u64 attributes = 0;
    /// The name's ASCII decode: each UTF-16LE unit below 0x80 as itself, a
    /// '?' for each that is not, NUL-padded.
    std::array<char, NAME_UNITS + 1> name = {};
};

/// What the header says about the entry array: where it starts, how many
/// entries, how many bytes each takes.
struct Header {
    u64 entries_lba = 0;
    u32 num_entries = 0;
    u32 entry_bytes = 0;
};

/// IEEE CRC-32, the reflected polynomial the specification names, seeded
/// |seed| so a caller can run one over blocks it reads one at a time.
[[nodiscard]]
auto crc32(std::span<u8 const> bytes, u32 const seed = 0) -> u32;

/// Parses the header out of |block|, one read of LBA 1: the signature, the
/// self-CRC, and the entry array's description. NotFound when the signature
/// is not a GPT's -- the caller's skip case, most disks are not GPT disks --
/// BadState when the CRC does not match, InvArgs when the fields cannot
/// describe a readable array.
[[nodiscard]]
auto parse_header(std::span<u8 const> block) -> ustl::Result<Header, ours::Status>;

/// Parses the entries out of |bytes| -- as many as fit, which need not be the
/// whole array, the caller reading block by block and calling once per block,
/// |bytes| starting at an entry's edge -- writing the used ones (a type GUID
/// not all zero) into |out| from |first|, and answers how many it wrote.
[[nodiscard]]
auto parse_entries(std::span<u8 const> bytes, usize const first, Header const &header,
                   std::span<Partition> out) -> ustl::Result<usize, ours::Status>;

/// Reads the table through |reader|, whose read_blocks(lba, count, out)
/// answers ours::Status and fills |out| with count * |block_bytes| bytes.
/// Writes the used partitions into |out| and answers their count. NotFound
/// when the disk carries no GPT, BadState when a CRC fails, InvArgs when the
/// block device's block is not the 512 bytes this reader is written for.
template <typename Reader>
auto read(Reader &reader, u32 const block_bytes, std::span<Partition> out)
    -> ustl::Result<usize, ours::Status> {
    // The buffers are one block each, so only this block size fits them; the
    // devices this project reaches are 512-byte, and a wider one is a change
    // to make when one arrives.
    constexpr auto LBA_BYTES = usize{512};
    if (block_bytes != LBA_BYTES) {
        return ustl::err(ours::Status::InvArgs);
    }

    // LBA 0: the protective MBR's boot signature is the cheapest tell that
    // this disk was partitioned at all.
    std::array<u8, LBA_BYTES> mbr{};
    if (auto const status = reader.read_blocks(0, 1, std::span{mbr}); status != ours::Status::Ok) {
        return ustl::err(status);
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        return ustl::err(ours::Status::NotFound);
    }

    // LBA 1: the header, which says what follows and vouches for it.
    std::array<u8, LBA_BYTES> header_block{};
    if (auto const status = reader.read_blocks(1, 1, std::span{header_block});
        status != ours::Status::Ok) {
        return ustl::err(status);
    }
    auto header = parse_header(header_block);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }

    // The entry array, block by block: the CRC accumulates over exactly the
    // bytes the header vouches for and none of the padding, and each block's
    // entries land in |out| as they are read.
    auto const per_block = LBA_BYTES / header->entry_bytes;
    auto entries_read = usize{0};
    auto used = usize{0};
    auto crc = u32{0};
    auto block = std::array<u8, LBA_BYTES>{};
    while (entries_read < header->num_entries) {
        auto const units = header->entry_bytes * (header->num_entries - entries_read < per_block
                                                      ? header->num_entries - entries_read
                                                      : per_block);
        if (auto const status = reader.read_blocks(header->entries_lba + entries_read / per_block,
                                                   1, std::span{block});
            status != ours::Status::Ok) {
            return ustl::err(status);
        }
        crc = crc32(std::span{block}.first(units), crc);
        auto const parsed = parse_entries(std::span{block}.first(units), used, *header, out);
        if (parsed.is_err()) {
            return ustl::err(parsed.unwrap_err());
        }
        used += parsed.unwrap();
        entries_read += per_block;
    }

    // The header's CRC of the array, at its field, is what the accumulated
    // one must come to: the last check, past which the table is believed.
    auto const recorded =
        static_cast<u32>(header_block[88]) | (static_cast<u32>(header_block[89]) << 8) |
        (static_cast<u32>(header_block[90]) << 16) | (static_cast<u32>(header_block[91]) << 24);
    if (crc != recorded) {
        return ustl::err(ours::Status::BadState);
    }
    return ustl::ok(used);
}
} // namespace ours_gpt

#endif // #ifndef OURS_GPT_OURS_GPT_HPP
