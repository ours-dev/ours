/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The GPT parser: the CRC-32 against its published known answer, the header
// parse and its refusals, the entry parse, and the whole read() through a
// fake reader holding a disk the tests lay out themselves.
#include <ours_gpt/ours_gpt.hpp>

#include <array>
#include <span>
#include <string_view>

#include <cstring>

#include <gtest/gtest.h>
#include <ours/status.hpp>

namespace {
using ours::Status;
using ours_gpt::Guid;
using ours_gpt::Header;
using ours_gpt::Partition;

constexpr auto LBA = usize{512};

/// Writes |value| little-endian into |block| at |offset|.
auto put32(std::span<u8> block, usize const offset, u32 const value) -> void {
    block[offset] = static_cast<u8>(value);
    block[offset + 1] = static_cast<u8>(value >> 8);
    block[offset + 2] = static_cast<u8>(value >> 16);
    block[offset + 3] = static_cast<u8>(value >> 24);
}

auto put64(std::span<u8> block, usize const offset, u64 const value) -> void {
    put32(block, offset, static_cast<u32>(value));
    put32(block, offset + 4, static_cast<u32>(value >> 32));
}

/// The GPT header the tests start from: revision 1.0, 128 entries of 128
/// bytes at LBA 2, CRCs of nothing yet.
struct HeaderBlock final {
    std::array<u8, LBA> bytes{};

    HeaderBlock(u32 const num_entries, u32 const entry_bytes) : bytes{} {
        std::memcpy(bytes.data(), ours_gpt::SIGNATURE.data(), ours_gpt::SIGNATURE.size());
        put32(bytes, 8, 0x0001'0000);
        put32(bytes, 12, 92);
        put64(bytes, 24, 1);  // MyLBA
        put64(bytes, 32, 34); // AlternateLBA
        put64(bytes, 72, 2);  // the entry array
        put32(bytes, 80, num_entries);
        put32(bytes, 84, entry_bytes);
    }

    /// Signs the header: the CRC over its first 92 bytes with the field
    /// zeroed, as the specification computes it.
    auto sign() -> void {
        put32(bytes, 16, 0);
        auto const crc = ours_gpt::crc32(std::span{bytes}.first(ours_gpt::HEADER_CRC_BYTES));
        put32(bytes, 16, crc);
    }
};

/// One entry at |index|: |type|, an id, the range, the name. |type| all zero
/// is the free slot the parse must skip.
auto put_entry(std::span<u8> array, usize const index, Guid const &type, u64 const first,
               u64 const last, std::string_view const name) -> void {
    auto const entry = array.subspan(index * 128, 128);
    std::memcpy(entry.data(), type.bytes.data(), type.bytes.size());
    for (auto unit = usize{0}; unit < name.size() && unit < ours_gpt::NAME_UNITS; ++unit) {
        entry[56 + unit * 2] = static_cast<u8>(name[unit]);
    }
    put64(entry, 32, first);
    put64(entry, 40, last);
}

/// A disk of |BLOCKS| blocks, laid out by the tests, read by ours_gpt::read.
template <usize BLOCKS>
class FakeReader final {
  public:
    auto block(u64 const lba) -> std::span<u8, LBA> {
        return std::span<u8, LBA>{m_blocks[lba]};
    }

    /// Copies |from| into the block |lba|, the write a span's assignment
    /// would not do.
    auto put(u64 const lba, std::array<u8, LBA> const &from) -> void {
        std::memcpy(m_blocks[lba].data(), from.data(), LBA);
    }

    auto read_blocks(u64 const lba, usize const count, std::span<u8> out) -> Status {
        if (lba + count > BLOCKS || out.size() < count * LBA) {
            return Status::OutOfRange;
        }
        for (auto index = usize{0}; index < count; ++index) {
            std::memcpy(out.data() + index * LBA, m_blocks[lba + index].data(), LBA);
        }
        return Status::Ok;
    }

  private:
    std::array<std::array<u8, LBA>, BLOCKS> m_blocks{};
};

/// A one-entry GPT over an eight-block disk: the layout every read() test
/// starts from, already signed. Four entries fill the array's one block:
/// two used, two free.
class OneEntry: public ::testing::Test {
  public:
    OneEntry() {
        m_disk.block(0)[510] = 0x55;
        m_disk.block(0)[511] = 0xAA;
        put_entry(m_disk.block(2), 0, Guid{ours_gpt::OURS_FS_TYPE_BYTES}, 6, 7, "ours-fs");
        put_entry(m_disk.block(2), 1, Guid{.bytes = {1}}, 8, 9, "other");
        auto header = HeaderBlock(4, 128);
        put32(header.bytes, 88, ours_gpt::crc32(m_disk.block(2)));
        header.sign();
        m_disk.put(1, header.bytes);
    }

    FakeReader<8> m_disk;
};

TEST(Crc32, KnownAnswer) {
    // The published known answer of the reflected CRC-32.
    auto const text = std::string_view{"123456789"};
    auto const bytes = std::span{reinterpret_cast<u8 const *>(text.data()), text.size()};
    EXPECT_EQ(ours_gpt::crc32(bytes), 0xCBF4'3926);
}

TEST(Crc32, Chains) {
    // Seeding with a first answer must be the CRC of the concatenation.
    auto const left = std::string_view{"hello "};
    auto const right = std::string_view{"world"};
    auto const whole = std::string_view{"hello world"};
    auto const span_of = [](std::string_view const text) {
        return std::span{reinterpret_cast<u8 const *>(text.data()), text.size()};
    };
    EXPECT_EQ(ours_gpt::crc32(span_of(right), ours_gpt::crc32(span_of(left))),
              ours_gpt::crc32(span_of(whole)));
}

TEST(ParseHeader, ReadsTheFields) {
    auto header = HeaderBlock(128, 128);
    header.sign();
    auto parsed = ours_gpt::parse_header(header.bytes);
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed->entries_lba, u64{2});
    EXPECT_EQ(parsed->num_entries, u32{128});
    EXPECT_EQ(parsed->entry_bytes, u32{128});
}

TEST(ParseHeader, RefusesWhatIsNotGpt) {
    auto header = HeaderBlock(4, 128);
    header.sign();
    header.bytes[0] = 'X';
    EXPECT_EQ(ours_gpt::parse_header(header.bytes).unwrap_err(), Status::NotFound);
}

TEST(ParseHeader, RefusesABrokenCrc) {
    auto header = HeaderBlock(4, 128);
    header.sign();
    header.bytes[72] ^= 0xFF; // a field the CRC covers, changed after signing
    EXPECT_EQ(ours_gpt::parse_header(header.bytes).unwrap_err(), Status::BadState);
}

TEST(ParseHeader, RefusesUnreadableFields) {
    auto zero_entries = HeaderBlock(0, 128);
    zero_entries.sign();
    EXPECT_EQ(ours_gpt::parse_header(zero_entries.bytes).unwrap_err(), Status::InvArgs);

    auto tiny_entries = HeaderBlock(4, 64);
    tiny_entries.sign();
    EXPECT_EQ(ours_gpt::parse_header(tiny_entries.bytes).unwrap_err(), Status::InvArgs);

    auto unknown_revision = HeaderBlock(4, 128);
    put32(unknown_revision.bytes, 8, 0x0002'0000);
    unknown_revision.sign();
    EXPECT_EQ(ours_gpt::parse_header(unknown_revision.bytes).unwrap_err(), Status::InvArgs);
}

TEST(ParseEntries, ReadsUsedAndSkipsFree) {
    auto header = Header{.entries_lba = 2, .num_entries = 4, .entry_bytes = 128};
    auto array = std::array<u8, 4 * 128>{};
    put_entry(array, 0, Guid{ours_gpt::OURS_FS_TYPE_BYTES}, 6, 7, "ours-fs");
    put_entry(array, 2, Guid{.bytes = {1}}, 8, 9, "other");

    auto partitions = std::array<Partition, 4>{};
    auto written = ours_gpt::parse_entries(array, 0, header, partitions);
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(written.unwrap(), usize{2});
    EXPECT_EQ(partitions[0].type, Guid{ours_gpt::OURS_FS_TYPE_BYTES});
    EXPECT_EQ(partitions[0].first_lba, u64{6});
    EXPECT_EQ(partitions[0].last_lba, u64{7});
    EXPECT_EQ(std::string_view{partitions[0].name.data()}, "ours-fs");
    EXPECT_EQ(std::string_view{partitions[1].name.data()}, "other");
}

TEST(ParseEntries, DecodesANonAsciiNameToASquare) {
    auto header = Header{.entries_lba = 2, .num_entries = 1, .entry_bytes = 128};
    auto array = std::array<u8, 128>{};
    put_entry(array, 0, Guid{.bytes = {1}}, 6, 7, "ou");
    // One unit above the ASCII range, U+2600, in the name's third place.
    array[56 + 2 * 2] = 0x00;
    array[56 + 2 * 2 + 1] = 0x26;

    auto partitions = std::array<Partition, 1>{};
    auto written = ours_gpt::parse_entries(array, 0, header, partitions);
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(std::string_view{partitions[0].name.data()}, "ou?");
}

TEST(ParseEntries, RefusesATooSmallOutput) {
    auto header = Header{.entries_lba = 2, .num_entries = 2, .entry_bytes = 128};
    auto array = std::array<u8, 2 * 128>{};
    put_entry(array, 0, Guid{.bytes = {1}}, 6, 7, "one");
    put_entry(array, 1, Guid{.bytes = {2}}, 8, 9, "two");

    auto partitions = std::array<Partition, 1>{};
    EXPECT_EQ(ours_gpt::parse_entries(array, 0, header, partitions).unwrap_err(),
              Status::BufferTooSmall);
}

TEST_F(OneEntry, ReadsTheWholeTable) {
    auto partitions = std::array<Partition, 8>{};
    auto read = ours_gpt::read(m_disk, 512, partitions);
    ASSERT_TRUE(read.is_ok());
    EXPECT_EQ(read.unwrap(), usize{2});
    EXPECT_EQ(partitions[0].type, Guid{ours_gpt::OURS_FS_TYPE_BYTES});
    EXPECT_EQ(std::string_view{partitions[0].name.data()}, "ours-fs");
    EXPECT_EQ(partitions[0].first_lba, u64{6});
}

TEST_F(OneEntry, SkipsADiskWithNoMbr) {
    m_disk.block(0)[510] = 0;
    auto partitions = std::array<Partition, 8>{};
    EXPECT_EQ(ours_gpt::read(m_disk, 512, partitions).unwrap_err(), Status::NotFound);
}

TEST_F(OneEntry, RefusesABrokenEntryCrc) {
    m_disk.block(2)[10] ^= 0xFF; // a byte the array's CRC covers, after signing
    auto partitions = std::array<Partition, 8>{};
    EXPECT_EQ(ours_gpt::read(m_disk, 512, partitions).unwrap_err(), Status::BadState);
}

TEST(Read, CoversAnArrayThatSpansBlocks) {
    // Six entries of 128 bytes fill one block and reach into the next: the
    // array is two blocks, its CRC run over 768 bytes and none of the
    // padding, and the entries on both sides of the edge are found.
    FakeReader<5> disk;
    disk.block(0)[510] = 0x55;
    disk.block(0)[511] = 0xAA;
    put_entry(disk.block(2), 3, Guid{ours_gpt::OURS_FS_TYPE_BYTES}, 20, 21, "edge-a");
    put_entry(disk.block(3), 0, Guid{.bytes = {2}}, 22, 23, "edge-b");
    auto header = HeaderBlock(6, 128);
    auto crc = ours_gpt::crc32(disk.block(2));
    crc = ours_gpt::crc32(std::span{disk.block(3)}.first(2 * 128), crc);
    put32(header.bytes, 88, crc);
    header.sign();
    disk.put(1, header.bytes);

    auto partitions = std::array<Partition, 8>{};
    auto read = ours_gpt::read(disk, 512, partitions);
    ASSERT_TRUE(read.is_ok());
    EXPECT_EQ(read.unwrap(), usize{2});
    EXPECT_EQ(std::string_view{partitions[0].name.data()}, "edge-a");
    EXPECT_EQ(std::string_view{partitions[1].name.data()}, "edge-b");
}
} // namespace
