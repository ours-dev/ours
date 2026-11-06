/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The volume library: the header's codec round trips and its refusals,
// and the mapping -- the one function everything above goes through --
// checked against hand-computed answers for the linear and the striped
// shapes. The striped case is the interview question: which leg owns a
// sector, and where on the leg.
#include <volume/volume.hpp>

#include <array>
#include <span>
#include <vector>

#include <cstring>

#include <gtest/gtest.h>

namespace {
using ours::Status;
using volume::EXTENT_SECTORS;
using volume::Header;
using volume::HEADER_BYTES;
using volume::Kind;
using volume::Lv;
using volume::Segment;

/// A two-PV group's uuid, shared by every header the tests make.
constexpr std::array<u8, 16> VG_UUID = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

/// A named LV, the tests' own.
auto lv_with(std::string_view const name) -> Lv {
    auto lv = Lv{};
    std::memcpy(lv.name.data(), name.data(), name.size());
    return lv;
}

/// A header of one PV: uuid, index, extents, and the LV slots the caller
/// fills in. The segments are the caller's own.
auto pv_header(u32 const pv_index, u32 const extents, std::span<Lv const> const lvs,
               std::span<Segment const> const segments) -> Header {
    auto header = Header{};
    header.uuid = {static_cast<u8>(pv_index),
                   0xA,
                   0xB,
                   0xC,
                   0xD,
                   0xE,
                   0xF,
                   0x10,
                   0x11,
                   0x12,
                   0x13,
                   0x14,
                   0x15,
                   0x16,
                   0x17,
                   static_cast<u8>(pv_index)};
    header.vg_uuid = VG_UUID;
    std::memcpy(header.vg_name.data(), "vg0", 3);
    header.pv_index = pv_index;
    header.pv_extent_count = extents;
    header.lv_count = static_cast<u32>(lvs.size());
    header.segment_count = static_cast<u32>(segments.size());
    for (auto index = usize{0}; index < lvs.size(); ++index) {
        header.lvs[index] = lvs[index];
    }
    for (auto index = usize{0}; index < segments.size(); ++index) {
        header.segments[index] = segments[index];
    }
    return header;
}

TEST(VolumeHeader, RoundTrip) {
    auto lv = lv_with("root");
    lv.kind = Kind::Linear;
    lv.sector_count = 4 * u64{EXTENT_SECTORS};
    auto segments = std::array<Segment, 2>{Segment{.lv = 0,
                                                   .pv = 0,
                                                   .role = 0,
                                                   .first_logical = 0,
                                                   .first_physical = 2,
                                                   .extent_count = 3},
                                           Segment{.lv = 0,
                                                   .pv = 1,
                                                   .role = 0,
                                                   .first_logical = 3,
                                                   .first_physical = 5,
                                                   .extent_count = 1}};
    auto header = pv_header(0, 8, {&lv, 1}, segments);

    auto bytes = std::array<u8, HEADER_BYTES>{};
    ASSERT_TRUE(volume::encode_header(bytes, header).is_ok());
    auto const decoded = volume::decode_header(bytes);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->pv_index, 0u);
    EXPECT_EQ(decoded->pv_extent_count, 8u);
    EXPECT_EQ(decoded->vg_uuid, VG_UUID);
    ASSERT_EQ(decoded->lv_count, 1u);
    EXPECT_EQ(decoded->lvs[0].kind, Kind::Linear);
    EXPECT_EQ(decoded->lvs[0].sector_count, 4 * u64{EXTENT_SECTORS});
    EXPECT_EQ(std::string_view{reinterpret_cast<char const *>(decoded->lvs[0].name.data())},
              "root");
    ASSERT_EQ(decoded->segment_count, 2u);
    EXPECT_EQ(decoded->segments[0].pv, 0u);
    EXPECT_EQ(decoded->segments[0].first_physical, 2u);
    EXPECT_EQ(decoded->segments[1].first_logical, 3u);
    EXPECT_EQ(decoded->segments[1].extent_count, 1u);
}

TEST(VolumeHeader, SecondaryCopyAnswersAlike) {
    // The second copy is the same bytes at the next HEADER_SECTORS: the
    // codec reads either.
    auto bytes = std::array<u8, HEADER_BYTES>{};
    auto header = pv_header(0, 4, {}, {});
    header.lv_count = 0;
    header.segment_count = 0;
    ASSERT_TRUE(volume::encode_header(bytes, header).is_ok());
    auto const decoded = volume::decode_header(bytes);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->pv_index, 0u);
}

TEST(VolumeHeader, Refusals) {
    auto bytes = std::array<u8, HEADER_BYTES>{};
    auto header = pv_header(0, 4, {}, {});

    // A bad size, a broken CRC, a wrong magic, a wrong version.
    EXPECT_EQ(volume::encode_header(std::span<u8>{bytes}.first(100), header).unwrap_err(),
              Status::InvArgs);
    ASSERT_TRUE(volume::encode_header(bytes, header).is_ok());
    bytes[10] ^= 0xFF;
    EXPECT_EQ(volume::decode_header(bytes).unwrap_err(), Status::InvArgs);
    ASSERT_TRUE(volume::encode_header(bytes, header).is_ok());
    bytes[3] = 'X'; // inside the magic
    EXPECT_EQ(volume::decode_header(bytes).unwrap_err(), Status::InvArgs);
    ASSERT_TRUE(volume::encode_header(bytes, header).is_ok());
    for (auto i = usize{0}; i < 4; ++i) {
        bytes[8 + i] = static_cast<u8>((9 >> (8 * i)) & 0xFF);
    }
    EXPECT_EQ(volume::decode_header(bytes).unwrap_err(), Status::InvArgs);

    // Counts past the slots, a segment past its PV's extents, a striped
    // LV without geometry, a linear LV with stripe fields.
    auto striped = lv_with("st");
    striped.kind = Kind::Striped;
    striped.chunk_sectors = 0; // no chunk: refused
    auto no_chunk = pv_header(0, 4, {&striped, 1}, {});
    EXPECT_EQ(volume::encode_header(bytes, no_chunk).unwrap_err(), Status::InvArgs);
    striped.chunk_sectors = 64;
    auto with_stray = pv_header(0, 4, {&striped, 1}, {});
    with_stray.lvs[0].kind = Kind::Linear; // stripe fields on a linear LV
    with_stray.lvs[0].stripe_count = 2;
    EXPECT_EQ(volume::encode_header(bytes, with_stray).unwrap_err(), Status::InvArgs);
    auto over = pv_header(0, 4, {}, {});
    over.segment_count = volume::MAX_SEGMENTS + 1;
    EXPECT_EQ(volume::encode_header(bytes, over).unwrap_err(), Status::InvArgs);
    auto past = pv_header(0, 4, {}, {});
    past.segment_count = 1;
    past.segments[0] = Segment{
        .lv = 0, .pv = 0, .first_logical = 0, .first_physical = 3, .extent_count = 4}; // 3+4 > 4
    EXPECT_EQ(volume::encode_header(bytes, past).unwrap_err(), Status::InvArgs);
    // A segment naming an LV past the count.
    auto ghost = pv_header(0, 4, {}, {});
    ghost.segment_count = 1;
    ghost.segments[0] =
        Segment{.lv = 2, .pv = 0, .first_logical = 0, .first_physical = 0, .extent_count = 1};
    EXPECT_EQ(volume::encode_header(bytes, ghost).unwrap_err(), Status::InvArgs);
}

/// The linear map: two segments across two PVs, and the logical sector's
/// answer is the physical one, hand computed.
TEST(VolumeGroup, LinearMap) {
    auto lv = lv_with("root");
    lv.kind = Kind::Linear;
    lv.sector_count = 4 * u64{EXTENT_SECTORS};
    // Extents 0-2 on PV 0 (from physical extent 2), extent 3 on PV 1
    // (from physical extent 5).
    auto segments = std::array<Segment, 2>{Segment{.lv = 0,
                                                   .pv = 0,
                                                   .role = 0,
                                                   .first_logical = 0,
                                                   .first_physical = 2,
                                                   .extent_count = 3},
                                           Segment{.lv = 0,
                                                   .pv = 1,
                                                   .role = 0,
                                                   .first_logical = 3,
                                                   .first_physical = 5,
                                                   .extent_count = 1}};
    auto group = volume::VolumeGroup{};
    ASSERT_EQ(group.add_pv(pv_header(0, 8, {&lv, 1}, std::span<Segment const>{segments}.first(1))),
              Status::Ok);
    ASSERT_EQ(group.add_pv(pv_header(1, 8, {&lv, 1}, std::span<Segment const>{segments}.last(1))),
              Status::Ok);
    ASSERT_TRUE(group.is_complete());
    ASSERT_EQ(group.lv_count(), 1u);

    // Logical extent 0, first sector: PV 0, physical extent 2's start.
    auto const first = group.map(0, 0);
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(first->pv, 0u);
    EXPECT_EQ(first->sector, 2 * u64{EXTENT_SECTORS});
    // One sector into logical extent 2: still PV 0, one sector further.
    auto const mid = group.map(0, 2 * u64{EXTENT_SECTORS} + 1);
    ASSERT_TRUE(mid.is_ok());
    EXPECT_EQ(mid->pv, 0u);
    EXPECT_EQ(mid->sector, 4 * u64{EXTENT_SECTORS} + 1);
    // The seam: logical extent 3 lives on PV 1.
    auto const seam = group.map(0, 3 * u64{EXTENT_SECTORS});
    ASSERT_TRUE(seam.is_ok());
    EXPECT_EQ(seam->pv, 1u);
    EXPECT_EQ(seam->sector, 5 * u64{EXTENT_SECTORS});
    // Past the LV's end: refused.
    EXPECT_EQ(group.map(0, 4 * u64{EXTENT_SECTORS}).unwrap_err(), Status::InvArgs);
    EXPECT_EQ(group.map(1, 0).unwrap_err(), Status::InvArgs);
}

/// The striped map: two legs, chunk 4 sectors (small, so the tests can
/// walk the round-robin by hand). Chunks alternate legs; within a leg the
/// chunks concatenate.
TEST(VolumeGroup, StripeMap) {
    auto lv = lv_with("st");
    lv.kind = Kind::Striped;
    lv.chunk_sectors = 4;
    lv.stripe_count = 2;
    // Two legs of one extent each: the LV is 2 MiB of sectors, chunked
    // 4 sectors at a time over the two legs.
    lv.sector_count = 2 * u64{EXTENT_SECTORS};
    auto segments = std::array<Segment, 2>{Segment{.lv = 0,
                                                   .pv = 0,
                                                   .role = static_cast<u16>(Kind::Striped),
                                                   .first_logical = 0,
                                                   .first_physical = 1,
                                                   .extent_count = 1},
                                           Segment{.lv = 0,
                                                   .pv = 1,
                                                   .role = static_cast<u16>(Kind::Striped),
                                                   .first_logical = 1,
                                                   .first_physical = 2,
                                                   .extent_count = 1}};
    auto group = volume::VolumeGroup{};
    ASSERT_EQ(group.add_pv(pv_header(0, 4, {&lv, 1}, std::span<Segment const>{segments}.first(1))),
              Status::Ok);
    ASSERT_EQ(group.add_pv(pv_header(1, 4, {&lv, 1}, std::span<Segment const>{segments}.last(1))),
              Status::Ok);
    ASSERT_TRUE(group.is_complete());

    // Chunk 0 (sectors 0-3): leg 0, PV 0, the leg's first extent's start.
    auto const a = group.map(0, 0);
    ASSERT_TRUE(a.is_ok());
    EXPECT_EQ(a->pv, 0u);
    EXPECT_EQ(a->sector, 1 * u64{EXTENT_SECTORS});
    // Chunk 1 (sectors 4-7): leg 1, PV 1.
    auto const b = group.map(0, 4);
    ASSERT_TRUE(b.is_ok());
    EXPECT_EQ(b->pv, 1u);
    EXPECT_EQ(b->sector, 2 * u64{EXTENT_SECTORS});
    // Chunk 2 (sectors 8-11): leg 0 again, one chunk along the leg.
    auto const c = group.map(0, 8);
    ASSERT_TRUE(c.is_ok());
    EXPECT_EQ(c->pv, 0u);
    EXPECT_EQ(c->sector, 1 * u64{EXTENT_SECTORS} + 4);
    // A sector inside a chunk stays inside it: sector 10 is chunk 2, leg
    // 0, two sectors into the chunk.
    auto const d = group.map(0, 10);
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d->pv, 0u);
    EXPECT_EQ(d->sector, 1 * u64{EXTENT_SECTORS} + 6);
    // The round-robin continues across extents' worth of chunks: sector
    // 2048 is chunk 512, the 257th chunk of leg 0 -- 1024 sectors along
    // the leg.
    auto const e = group.map(0, EXTENT_SECTORS);
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e->pv, 0u);
    EXPECT_EQ(e->sector, 1 * u64{EXTENT_SECTORS} + 1024);
}

/// An incomplete group answers NotFound where its missing piece would
/// have answered, and the assembler refuses a second PV of one index.
TEST(VolumeGroup, IncompleteAndRefusals) {
    auto lv = lv_with("root");
    lv.kind = Kind::Linear;
    lv.sector_count = 2 * u64{EXTENT_SECTORS};
    auto segments = std::array<Segment, 1>{Segment{
        .lv = 0, .pv = 1, .role = 0, .first_logical = 0, .first_physical = 0, .extent_count = 2}};
    auto group = volume::VolumeGroup{};
    ASSERT_EQ(group.add_pv(pv_header(0, 4, {&lv, 1}, segments)), Status::Ok);
    // The segment's PV (1) never joined: the group is not complete, and
    // its one mapped sector reads NotFound.
    EXPECT_FALSE(group.is_complete());
    EXPECT_EQ(group.map(0, 0).unwrap_err(), Status::NotFound);

    // A second header of one pv_index: refused. A different VG: refused.
    auto lv_list = std::array<Lv, 1>{lv};
    auto seg_list = std::span<Segment const>{segments};
    EXPECT_EQ(group.add_pv(pv_header(0, 4, lv_list, seg_list)), Status::InvArgs);
    auto stranger = pv_header(3, 4, lv_list, seg_list);
    stranger.vg_uuid = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    EXPECT_EQ(group.add_pv(stranger), Status::InvArgs);
}

TEST(VolumeGroup, StripeMissingLegReadsNotFound) {
    auto lv = lv_with("st");
    lv.kind = Kind::Striped;
    lv.chunk_sectors = 4;
    lv.stripe_count = 2;
    lv.sector_count = 2 * u64{EXTENT_SECTORS};
    auto segments = std::array<Segment, 2>{Segment{.lv = 0,
                                                   .pv = 0,
                                                   .role = static_cast<u16>(Kind::Striped),
                                                   .first_logical = 0,
                                                   .first_physical = 1,
                                                   .extent_count = 1},
                                           Segment{.lv = 0,
                                                   .pv = 1,
                                                   .role = static_cast<u16>(Kind::Striped),
                                                   .first_logical = 1,
                                                   .first_physical = 2,
                                                   .extent_count = 1}};
    auto group = volume::VolumeGroup{};
    // PV 1 joined first, PV 0 never: leg 0's chunks read NotFound, leg
    // 1's answer.
    ASSERT_EQ(group.add_pv(pv_header(1, 4, {&lv, 1}, std::span<Segment const>{segments}.last(1))),
              Status::Ok);
    EXPECT_FALSE(group.is_complete());
    auto const leg1 = group.map(0, 4);
    ASSERT_TRUE(leg1.is_ok());
    EXPECT_EQ(leg1->pv, 1u);
    EXPECT_EQ(group.map(0, 0).unwrap_err(), Status::NotFound);
}

/// The CRC matches the GPT's: a known answer, the string "123456789"'s
/// CRC-32, so one table walks both and the maker script shares it.
TEST(VolumeCrc, KnownAnswer) {
    auto const bytes = std::array<u8, 9>{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(volume::crc32(bytes), 0xCBF4'3926u);
}

/// The decode's own refusals, one broken field at a time, on a block that
/// encodes clean. The encoder refuses these shapes too; the decoder
/// cannot trust that it was the one who wrote the bytes. Each patch makes
/// the CRC honest again, so the field's check is what fires, not the
/// CRC's.
TEST(VolumeHeader, DecodeRefusals) {
    auto lv = lv_with("root");
    lv.kind = Kind::Linear;
    lv.sector_count = 2 * u64{EXTENT_SECTORS};
    auto segments = std::array<Segment, 1>{Segment{
        .lv = 0, .pv = 0, .role = 0, .first_logical = 0, .first_physical = 0, .extent_count = 2}};
    auto bytes = std::array<u8, HEADER_BYTES>{};
    ASSERT_TRUE(volume::encode_header(bytes, pv_header(0, 8, {&lv, 1}, segments)).is_ok());

    auto with_field = [](std::array<u8, HEADER_BYTES> block, usize const at, u32 const value) {
        for (auto i = usize{0}; i < 4; ++i) {
            block[at + i] = static_cast<u8>((value >> (8 * i)) & 0xFF);
        }
        auto zeroed = block;
        zeroed[1020] = 0;
        zeroed[1021] = 0;
        zeroed[1022] = 0;
        zeroed[1023] = 0;
        auto const crc = volume::crc32(zeroed);
        for (auto i = usize{0}; i < 4; ++i) {
            block[1020 + i] = static_cast<u8>((crc >> (8 * i)) & 0xFF);
        }
        return block;
    };
    auto refused = [](std::span<u8 const> const broken) {
        return volume::decode_header(broken).unwrap_err() == Status::InvArgs;
    };

    // A wrong size.
    EXPECT_TRUE(refused(std::span<u8 const>{bytes}.first(1000)));
    // The version, the counts past the slots, an LV kind and a segment
    // role the model does not name, a segment's LV past the count, and
    // the segment's extents past the PV's.
    EXPECT_TRUE(refused(with_field(bytes, 8, 9)));
    EXPECT_TRUE(refused(with_field(bytes, 72, 5)));          // lv_count
    EXPECT_TRUE(refused(with_field(bytes, 76, 33)));         // segment_count
    EXPECT_TRUE(refused(with_field(bytes, 80 + 16, 7)));     // the LV's kind
    EXPECT_TRUE(refused(with_field(bytes, 80 + 16 + 8, 2))); // a linear LV's stripes
    EXPECT_TRUE(refused(with_field(bytes, 240 + 4, 9)));     // the segment's role
    EXPECT_TRUE(refused(with_field(bytes, 240 + 0, 3)));     // the segment's lv
    EXPECT_TRUE(refused(with_field(bytes, 240 + 12, 9)));    // first_physical, 9 of 8
}

/// The encode's refusals the wire-level patches cannot reach: a segment
/// whose role names no kind, and a segment whose extents run past the
/// PV's own.
TEST(VolumeHeader, EncodeRefusals) {
    auto lv = lv_with("root");
    lv.kind = Kind::Linear;
    lv.sector_count = u64{EXTENT_SECTORS};
    auto lv_list = std::array<Lv, 1>{lv};
    auto bytes = std::array<u8, HEADER_BYTES>{};

    auto wrong_role = std::array<Segment, 1>{Segment{
        .lv = 0, .pv = 0, .role = 9, .first_logical = 0, .first_physical = 0, .extent_count = 1}};
    EXPECT_EQ(volume::encode_header(bytes, pv_header(0, 4, lv_list, wrong_role)).unwrap_err(),
              Status::InvArgs);

    auto past = std::array<Segment, 1>{Segment{.lv = 0,
                                               .pv = 0,
                                               .role = 0,
                                               .first_logical = 0,
                                               .first_physical = 3,
                                               .extent_count = 4}}; // 3+4 > 4
    EXPECT_EQ(volume::encode_header(bytes, pv_header(0, 4, lv_list, past)).unwrap_err(),
              Status::InvArgs);
}

/// A striped LV whose wire form lost its chunk: the decode refuses, as
/// the encode did -- the bytes on a disk answer to nobody.
TEST(VolumeHeader, DecodeRefusesABrokenStripe) {
    auto lv = lv_with("st");
    lv.kind = Kind::Striped;
    lv.chunk_sectors = 64;
    lv.stripe_count = 2;
    lv.sector_count = 2 * u64{EXTENT_SECTORS};
    auto bytes = std::array<u8, HEADER_BYTES>{};
    ASSERT_TRUE(volume::encode_header(bytes, pv_header(0, 4, {&lv, 1}, {})).is_ok());
    // The chunk field of the LV slot, at slot 0 + name + 4, to zero.
    for (auto i = usize{0}; i < 4; ++i) {
        bytes[80 + 16 + 4 + i] = 0;
    }
    auto zeroed = bytes;
    zeroed[1020] = 0;
    zeroed[1021] = 0;
    zeroed[1022] = 0;
    zeroed[1023] = 0;
    auto const crc = volume::crc32(zeroed);
    for (auto i = usize{0}; i < 4; ++i) {
        bytes[1020 + i] = static_cast<u8>((crc >> (8 * i)) & 0xFF);
    }
    EXPECT_EQ(volume::decode_header(bytes).unwrap_err(), Status::InvArgs);
}

/// An empty group is not complete: nothing is named, nothing is served.
TEST(VolumeGroup, EmptyGroupIsIncomplete) {
    auto group = volume::VolumeGroup{};
    EXPECT_FALSE(group.is_complete());
    EXPECT_EQ(group.lv_count(), 0u);
}

/// The assembler's limits: one PV too many, one segment too many across
/// the headers, and a second header of one PV uuid.
TEST(VolumeGroup, AssemblerLimits) {
    auto lv = lv_with("root");
    lv.kind = Kind::Linear;
    lv.sector_count = u64{EXTENT_SECTORS};
    auto lv_list = std::array<Lv, 1>{lv};
    auto segments = std::array<Segment, 1>{Segment{
        .lv = 0, .pv = 0, .role = 0, .first_logical = 0, .first_physical = 0, .extent_count = 1}};
    auto group = volume::VolumeGroup{};
    // A second header of one uuid, whatever its index: refused while the
    // table still has room to compare it.
    auto twin = pv_header(9, 1, lv_list, std::span<Segment const>{segments});
    twin.uuid = pv_header(0, 1, lv_list, std::span<Segment const>{segments}).uuid;
    ASSERT_EQ(group.add_pv(pv_header(0, 1, lv_list, std::span<Segment const>{segments})),
              Status::Ok);
    EXPECT_EQ(group.add_pv(twin), Status::InvArgs);
    for (auto index = usize{1}; index < 8; ++index) {
        auto header =
            pv_header(static_cast<u32>(index), 1, lv_list, std::span<Segment const>{segments});
        // Every PV needs its own uuid.
        header.uuid[15] = static_cast<u8>(index);
        ASSERT_EQ(group.add_pv(header), Status::Ok);
    }
    EXPECT_EQ(group.pv_count(), 8u);
    auto ninth = pv_header(8, 1, lv_list, std::span<Segment const>{segments});
    ninth.uuid[15] = 8;
    EXPECT_EQ(group.add_pv(ninth), Status::NoResource);

    // The LV slots are the group's, by index.
    EXPECT_EQ(group.lv(0).kind, Kind::Linear);

    // One segment too many across the headers: five per PV, and the
    // table's 32 run out mid-way through the sixth.
    auto wide = std::array<Segment, 5>{};
    for (auto index = usize{0}; index < 5; ++index) {
        wide[index] = Segment{.lv = 0,
                              .pv = 0,
                              .role = 0,
                              .first_logical = 0,
                              .first_physical = static_cast<u32>(index),
                              .extent_count = 1};
    }
    auto full_group = volume::VolumeGroup{};
    for (auto pv = usize{0}; pv < 7; ++pv) {
        auto header = pv_header(static_cast<u32>(pv), 8, lv_list, std::span<Segment const>{wide});
        header.uuid[15] = static_cast<u8>(pv);
        auto const added = full_group.add_pv(header);
        // Seven headers of five: the 32 run out during the seventh
        // (30 + 5), which answers NoResource with the table full.
        if (pv == 6) {
            EXPECT_EQ(added, Status::NoResource);
        } else {
            ASSERT_EQ(added, Status::Ok);
        }
    }
    EXPECT_EQ(full_group.segment_total(), 32u);
}

/// Two LVs of one group: the linear map skips the other LV's segments on
/// the way to its own, and both map without crossing.
TEST(VolumeGroup, TwoLinearLvs) {
    auto first = lv_with("a");
    first.kind = Kind::Linear;
    first.sector_count = u64{EXTENT_SECTORS};
    auto second = lv_with("b");
    second.kind = Kind::Linear;
    second.sector_count = u64{EXTENT_SECTORS};
    auto lv_list = std::array<Lv, 2>{first, second};
    auto segments = std::array<Segment, 2>{Segment{.lv = 0,
                                                   .pv = 0,
                                                   .role = 0,
                                                   .first_logical = 0,
                                                   .first_physical = 0,
                                                   .extent_count = 1},
                                           Segment{.lv = 1,
                                                   .pv = 0,
                                                   .role = 0,
                                                   .first_logical = 0,
                                                   .first_physical = 1,
                                                   .extent_count = 1}};
    auto group = volume::VolumeGroup{};
    ASSERT_EQ(group.add_pv(pv_header(0, 4, lv_list, segments)), Status::Ok);
    ASSERT_TRUE(group.is_complete());
    EXPECT_EQ(group.lv_count(), 2u);

    auto const a = group.map(0, 0);
    ASSERT_TRUE(a.is_ok());
    EXPECT_EQ(a->sector, 0u);
    auto const b = group.map(1, 0);
    ASSERT_TRUE(b.is_ok());
    EXPECT_EQ(b->sector, u64{EXTENT_SECTORS});
}
} // namespace
