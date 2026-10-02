/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Plan::build(): how the PT_LOAD segments of a program are mapped. The
/// specification is the comment on Plan::make(), in src/plan.cpp.
#include <launch/plan.hpp>

#include <array>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace {
using launch::Copy;
using launch::Mapping;
using launch::Perm;
using launch::PF_R;
using launch::PF_W;
using launch::PF_X;
using launch::Plan;
using launch::Relro;
using launch::Segment;
using launch::Source;
using ours::Status;

constexpr u64 FILE_SIZE = 0x4000;

auto build(std::vector<Segment> const &segments, Relro const relro, u64 const file_size = FILE_SIZE)
    -> ustl::Result<Plan, Status> {
    return Plan::build(segments, relro, file_size);
}

/// |name| is the plan build() gives for the rest of the arguments; the test
/// stops if build() refuses them.
#define PLAN_OF(name, ...)                                                                         \
    auto const name##_result = build(__VA_ARGS__);                                                 \
    ASSERT_TRUE(name##_result.is_ok())                                                             \
        << "build() answered " << static_cast<int>(name##_result.unwrap_err());                    \
    auto const &name = name##_result.unwrap()

void expect_mapping(Plan const &plan, usize const index, Source const source, u32 const segment,
                    u64 const vaddr, u64 const size, u64 const vmo_offset, Perm const perm) {
    ASSERT_LT(index, plan.mappings().size());
    auto const &m = plan.mappings()[index];
    EXPECT_EQ(m.source, source) << "mapping " << index;
    EXPECT_EQ(m.segment, segment) << "mapping " << index;
    EXPECT_EQ(m.vaddr, vaddr) << "mapping " << index;
    EXPECT_EQ(m.size, size) << "mapping " << index;
    EXPECT_EQ(m.vmo_offset, vmo_offset) << "mapping " << index;
    EXPECT_EQ(m.perm, perm) << "mapping " << index;
}

void expect_snapshot(Plan const &plan, u32 const segment, u64 const file_offset, u64 const size) {
    EXPECT_EQ(plan.snapshot(segment).file_offset, file_offset) << "segment " << segment;
    EXPECT_EQ(plan.snapshot(segment).size, size) << "segment " << segment;
}

void expect_copy(Plan const &plan, usize const index, u32 const segment, u64 const vmo_offset,
                 u64 const file_offset, u64 const size) {
    ASSERT_LT(index, plan.copies().size());
    auto const &c = plan.copies()[index];
    EXPECT_EQ(c.segment, segment);
    EXPECT_EQ(c.vmo_offset, vmo_offset);
    EXPECT_EQ(c.file_offset, file_offset);
    EXPECT_EQ(c.size, size);
}

/// What a linker gives a small program with separate code and RELRO: headers
/// and read-only data, code, more read-only data, and a writable segment that
/// starts with the RELRO and ends in .bss.
///
///   S0  R    0x0000  0x600 bytes           the ELF and program headers, .dynsym
///   S1  RX   0x1000  0x340                 .text
///   S2  R    0x2000  0x090                 .rodata
///   S3  RW   0x3f00  0x180 in the file,    RELRO (.got and the like) 0x100 long,
///                    0x2180 in memory      then .data, then 0x2000 of .bss
std::vector<Segment> const TYPICAL = {
    {0x0000, 0x0000, 0x600, 0x600, PF_R},
    {0x1000, 0x1000, 0x340, 0x340, PF_R | PF_X},
    {0x2000, 0x2000, 0x090, 0x090, PF_R},
    {0x3f00, 0x2f00, 0x180, 0x2180, PF_R | PF_W},
};
constexpr Relro TYPICAL_RELRO{0x3f00, 0x100};

TEST(PlanMappings, TypicalLayout) {
    PLAN_OF(plan, TYPICAL, TYPICAL_RELRO);

    // The three segments that are not writable come from the file, a page each.
    ASSERT_EQ(plan.mappings().size(), 5U);
    expect_mapping(plan, 0, Source::File, 0, 0x0000, 0x1000, 0x0000, Perm::R);
    expect_mapping(plan, 1, Source::File, 1, 0x1000, 0x1000, 0x1000, Perm::RX);
    expect_mapping(plan, 2, Source::File, 2, 0x2000, 0x1000, 0x2000, Perm::R);

    // The writable one. Its file bytes run from 0x3f00 to 0x4080, so the page
    // 0x3000 holds file bytes only and comes from the snapshot; its last 0x100
    // bytes are RELRO and are the whole of what is in it, so it is read-only
    // once relocated. The page 0x4000 has the last 0x80 file bytes and then
    // .bss, so it and the .bss pages after it are zero-fill, with those 0x80
    // bytes copied in.
    expect_mapping(plan, 3, Source::Snapshot, 3, 0x3000, 0x1000, 0x0000, Perm::R);
    expect_mapping(plan, 4, Source::Zero, 3, 0x4000, 0x3000, 0x0000, Perm::RW);

    expect_snapshot(plan, 3, 0x2000, 0x1000);
    EXPECT_EQ(plan.zero_size(3), 0x3000U);
    ASSERT_EQ(plan.copies().size(), 1U);
    expect_copy(plan, 0, 3, 0x000, 0x3000, 0x80);

    for (u32 segment = 0; segment < 3; ++segment) {
        expect_snapshot(plan, segment, 0, 0);
        EXPECT_EQ(plan.zero_size(segment), 0U) << "segment " << segment;
    }
}

TEST(PlanMappings, NoRelroIsOneWritableMappingPerVmo) {
    PLAN_OF(plan, TYPICAL, {});
    ASSERT_EQ(plan.mappings().size(), 5U);
    expect_mapping(plan, 3, Source::Snapshot, 3, 0x3000, 0x1000, 0x0000, Perm::RW);
    expect_mapping(plan, 4, Source::Zero, 3, 0x4000, 0x3000, 0x0000, Perm::RW);
}

TEST(PlanMappings, RelroThatIsNotAWholePageIsNotProtected) {
    // 0x800 bytes of RELRO at the start of a page: the page also holds what
    // follows, which is written to, so it cannot be read-only.
    std::vector<Segment> const segments = {{0x3000, 0x2000, 0x1000, 0x1000, PF_R | PF_W}};
    PLAN_OF(plan, segments, {0x3000, 0x800});
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::Snapshot, 0, 0x3000, 0x1000, 0x0000, Perm::RW);
}

TEST(PlanMappings, RelroThatIsTheWholeSegmentLeavesNothingWritable) {
    std::vector<Segment> const segments = {{0x3000, 0x2000, 0x2000, 0x2000, PF_R | PF_W}};
    PLAN_OF(plan, segments, {0x3000, 0x2000});
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::Snapshot, 0, 0x3000, 0x2000, 0x0000, Perm::R);
}

TEST(PlanMappings, RelroSpanningPagesRoundsItsEndDown) {
    // RELRO from the start of the segment to 0x3a00 (offset 0x2a00): two whole
    // pages, then a page it only starts to fill.
    std::vector<Segment> const segments = {{0x1000, 0x0000, 0x3000, 0x3000, PF_R | PF_W}};
    PLAN_OF(plan, segments, {0x1000, 0x2a00});
    ASSERT_EQ(plan.mappings().size(), 2U);
    expect_mapping(plan, 0, Source::Snapshot, 0, 0x1000, 0x2000, 0x0000, Perm::R);
    expect_mapping(plan, 1, Source::Snapshot, 0, 0x3000, 0x1000, 0x2000, Perm::RW);
}

TEST(PlanMappings, ProtectionFollowsTheFlags) {
    // Execute without read is still readable when mapped, and write without
    // read is still a writable mapping.
    PLAN_OF(executable, {{0x0, 0x0, 0x10, 0x10, PF_X}}, {});
    expect_mapping(executable, 0, Source::File, 0, 0x0, 0x1000, 0x0, Perm::RX);
    PLAN_OF(writable, {{0x0, 0x0, 0x10, 0x10, PF_W}}, {});
    expect_mapping(writable, 0, Source::Snapshot, 0, 0x0, 0x1000, 0x0, Perm::RW);
}

TEST(PlanMappings, WritableSegmentWithNothingInTheFileIsAllZero) {
    // A pure .bss: a zero-fill VMO, mapped, no snapshot and nothing to copy.
    PLAN_OF(plan, {{0x5000, 0x0, 0x0, 0x1800, PF_R | PF_W}}, {});
    EXPECT_EQ(plan.copies().size(), 0U);
    expect_snapshot(plan, 0, 0, 0);
    EXPECT_EQ(plan.zero_size(0), 0x2000U);
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::Zero, 0, 0x5000, 0x2000, 0x0, Perm::RW);
}

TEST(PlanMappings, WritableSegmentWithoutBssIsAllSnapshot) {
    // mem_size == file_size: the bytes of the last page past the data are past
    // mem_size too, and nothing promises what they hold, so every page comes
    // from the file and nothing is copied, even where the data ends mid-page.
    PLAN_OF(plan, {{0x2000, 0x1000, 0x1800, 0x1800, PF_R | PF_W}}, {});
    expect_snapshot(plan, 0, 0x1000, 0x2000);
    EXPECT_EQ(plan.zero_size(0), 0U);
    EXPECT_EQ(plan.copies().size(), 0U);
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::Snapshot, 0, 0x2000, 0x2000, 0x0, Perm::RW);
}

TEST(PlanMappings, DataEndingOnAPageBoundaryNeedsNoCopy) {
    // The file bytes fill the page 0x3000 exactly, and .bss starts on the next
    // one: the snapshot and the zero-fill VMO meet there, and nothing is copied.
    PLAN_OF(plan, {{0x3000, 0x1000, 0x1000, 0x3000, PF_R | PF_W}}, {});
    expect_snapshot(plan, 0, 0x1000, 0x1000);
    EXPECT_EQ(plan.zero_size(0), 0x2000U);
    EXPECT_EQ(plan.copies().size(), 0U);
    ASSERT_EQ(plan.mappings().size(), 2U);
    expect_mapping(plan, 0, Source::Snapshot, 0, 0x3000, 0x1000, 0x0, Perm::RW);
    expect_mapping(plan, 1, Source::Zero, 0, 0x4000, 0x2000, 0x0, Perm::RW);
}

TEST(PlanMappings, DataOverSeveralPagesEndingMidPage) {
    // 0x2800 file bytes from 0x1000: two whole pages from the snapshot, then
    // the page that holds the last 0x800 of them and the start of .bss.
    PLAN_OF(plan, {{0x1000, 0x0, 0x2800, 0x4000, PF_R | PF_W}}, {});
    expect_snapshot(plan, 0, 0x0, 0x2000);
    EXPECT_EQ(plan.zero_size(0), 0x2000U);
    ASSERT_EQ(plan.copies().size(), 1U);
    expect_copy(plan, 0, 0, 0x0, 0x2000, 0x800);
    ASSERT_EQ(plan.mappings().size(), 2U);
    expect_mapping(plan, 0, Source::Snapshot, 0, 0x1000, 0x2000, 0x0, Perm::RW);
    expect_mapping(plan, 1, Source::Zero, 0, 0x3000, 0x2000, 0x0, Perm::RW);
}

TEST(PlanMappings, DataThatStartsAndEndsInOnePageWithBss) {
    // 0x80 bytes at 0x3100, then .bss to 0x5100: the data never fills a page,
    // so there is no snapshot. The bytes are copied to where they sit in that
    // page, 0x100 into the zero-fill VMO, and the bytes before them read as zero.
    PLAN_OF(plan, {{0x3100, 0x1100, 0x80, 0x2000, PF_R | PF_W}}, {});
    expect_snapshot(plan, 0, 0, 0);
    EXPECT_EQ(plan.zero_size(0), 0x3000U);
    ASSERT_EQ(plan.copies().size(), 1U);
    expect_copy(plan, 0, 0, 0x100, 0x1100, 0x80);
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::Zero, 0, 0x3000, 0x3000, 0x0, Perm::RW);
}

TEST(PlanMappings, RelroThatEndsBeforeTheZeroFillPart) {
    // The other order of the two cuts: RELRO ends inside the snapshot, so the
    // snapshot is split by protection and the zero-fill part is writable.
    PLAN_OF(plan, {{0x1000, 0x0, 0x2800, 0x4000, PF_R | PF_W}}, {0x1000, 0x1000});
    ASSERT_EQ(plan.mappings().size(), 3U);
    expect_mapping(plan, 0, Source::Snapshot, 0, 0x1000, 0x1000, 0x0, Perm::R);
    expect_mapping(plan, 1, Source::Snapshot, 0, 0x2000, 0x1000, 0x1000, Perm::RW);
    expect_mapping(plan, 2, Source::Zero, 0, 0x3000, 0x2000, 0x0, Perm::RW);
}

TEST(PlanMappings, RelroSegmentAsLldLinksIt) {
    // The writable segment of the shell as LLD 23 links it: 0x468 bytes in
    // the file, 0x6c0 in memory, because RELRO ends in .relro_padding, which
    // takes no room in the file and rounds RELRO up to the page boundary.
    // PT_GNU_RELRO's p_memsz is 0x6c0 and its p_filesz 0x468. The data starts
    // and ends in one page, so it is all zero-fill, and that page is wholly
    // RELRO by p_memsz.
    PLAN_OF(plan, {{0xe940, 0xc940, 0x468, 0x6c0, PF_R | PF_W}}, {0xe940, 0x6c0}, 0x40c00);
    expect_snapshot(plan, 0, 0, 0);
    EXPECT_EQ(plan.zero_size(0), 0x1000U);
    ASSERT_EQ(plan.copies().size(), 1U);
    expect_copy(plan, 0, 0, 0x940, 0xc940, 0x468);
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::Zero, 0, 0xe000, 0x1000, 0x0, Perm::R);

    // With p_filesz the page is not wholly RELRO, and stays writable.
    PLAN_OF(by_filesz, {{0xe940, 0xc940, 0x468, 0x6c0, PF_R | PF_W}}, {0xe940, 0x468}, 0x40c00);
    expect_mapping(by_filesz, 0, Source::Zero, 0, 0xe000, 0x1000, 0x0, Perm::RW);
}

TEST(PlanMappings, SnapshotMayEndInTheFilesLastPartialPage) {
    // No .bss and the data ends at the end of the file, mid-page: the snapshot
    // still covers the whole last page, which the file VMO has (a VMO is whole
    // pages, and a snapshot reads zero past its parent's end).
    PLAN_OF(plan, {{0x2000, 0x1000, 0x1800, 0x1800, PF_R | PF_W}}, {}, 0x2800);
    expect_snapshot(plan, 0, 0x1000, 0x2000);
    EXPECT_EQ(plan.copies().size(), 0U);
}

TEST(PlanMappings, RelroThatReachesIntoTheZeroFillPart) {
    // RELRO 0x2000 long over a page of file bytes and a page of .bss: the cut
    // between the snapshot and the zero-fill VMO and the end of RELRO fall in
    // different places, so the segment gives three mappings.
    PLAN_OF(plan, {{0x3000, 0x1000, 0x1000, 0x3000, PF_R | PF_W}}, {0x3000, 0x2000});
    ASSERT_EQ(plan.mappings().size(), 3U);
    expect_mapping(plan, 0, Source::Snapshot, 0, 0x3000, 0x1000, 0x0, Perm::R);
    expect_mapping(plan, 1, Source::Zero, 0, 0x4000, 0x1000, 0x0, Perm::R);
    expect_mapping(plan, 2, Source::Zero, 0, 0x5000, 0x1000, 0x1000, Perm::RW);
}

TEST(PlanMappings, SegmentThatStartsMidPageIsMappedFromItsPage) {
    // A read-only segment at 0x1234 in the file and in memory: the page it is
    // in is mapped, from the start of that page of the file.
    PLAN_OF(plan, {{0x1234, 0x1234, 0x100, 0x100, PF_R}}, {});
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::File, 0, 0x1000, 0x1000, 0x1000, Perm::R);
}

TEST(PlanMappings, SegmentThatRunsIntoTheNextPage) {
    // 0x1f00 + 0x200 crosses a page boundary: two pages.
    PLAN_OF(plan, {{0x1f00, 0x1f00, 0x200, 0x200, PF_R}}, {});
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::File, 0, 0x1000, 0x2000, 0x1000, Perm::R);
}

TEST(PlanMappings, TwoWritableSegmentsEachHaveTheirOwnVmo) {
    std::vector<Segment> const segments = {
        {0x1000, 0x2000, 0x10, 0x10, PF_R | PF_W},
        {0x3000, 0x3000, 0x20, 0x3000, PF_R | PF_W},
    };
    PLAN_OF(plan, segments, {});
    // The first has no .bss, so its one page is the snapshot; the second's 0x20
    // file bytes share their page with .bss, so it is all zero-fill.
    ASSERT_EQ(plan.mappings().size(), 2U);
    expect_mapping(plan, 0, Source::Snapshot, 0, 0x1000, 0x1000, 0x0, Perm::RW);
    expect_mapping(plan, 1, Source::Zero, 1, 0x3000, 0x3000, 0x0, Perm::RW);
    ASSERT_EQ(plan.copies().size(), 1U);
    expect_copy(plan, 0, 1, 0x0, 0x3000, 0x20);
    expect_snapshot(plan, 0, 0x2000, 0x1000);
    EXPECT_EQ(plan.zero_size(0), 0U);
    expect_snapshot(plan, 1, 0, 0);
    EXPECT_EQ(plan.zero_size(1), 0x3000U);
}

TEST(PlanMappings, RelroBelongsToTheSegmentItStartsIn) {
    // Two writable segments; RELRO is the start of the second. The first is
    // untouched by it.
    std::vector<Segment> const segments = {
        {0x1000, 0x1000, 0x1000, 0x1000, PF_R | PF_W},
        {0x3000, 0x2000, 0x1000, 0x1000, PF_R | PF_W},
    };
    PLAN_OF(plan, segments, {0x3000, 0x1000});
    ASSERT_EQ(plan.mappings().size(), 2U);
    expect_mapping(plan, 0, Source::Snapshot, 0, 0x1000, 0x1000, 0x0, Perm::RW);
    expect_mapping(plan, 1, Source::Snapshot, 1, 0x3000, 0x1000, 0x0, Perm::R);
}

TEST(PlanMappings, MaximumSegments) {
    std::vector<Segment> segments;
    for (u64 i = 0; i < Plan::MAX_SEGMENTS; ++i) {
        segments.push_back({i * 0x1000, i * 0x1000, 0x10, 0x10, PF_R});
    }
    PLAN_OF(plan, segments, {}, Plan::MAX_SEGMENTS * 0x1000);
    EXPECT_EQ(plan.mappings().size(), Plan::MAX_SEGMENTS);
}

// ---- What is refused: build() answers the error, and there is no Plan.

void expect_refused(std::vector<Segment> const &segments, Relro const relro, Status const status,
                    u64 const file_size = FILE_SIZE) {
    auto const result = build(segments, relro, file_size);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err(), status);
}

TEST(PlanMappings, RefusesNoSegments) {
    expect_refused({}, {}, Status::InvArgs);
}

TEST(PlanMappings, RefusesTooManySegments) {
    std::vector<Segment> segments;
    for (u64 i = 0; i <= Plan::MAX_SEGMENTS; ++i) {
        segments.push_back({i * 0x1000, i * 0x1000, 0x10, 0x10, PF_R});
    }
    expect_refused(segments, {}, Status::OutOfRange, 0x10000);
}

TEST(PlanMappings, RefusesAnEmptySegment) {
    expect_refused({{0x0, 0x0, 0x0, 0x0, PF_R}}, {}, Status::InvArgs);
}

TEST(PlanMappings, RefusesMoreInTheFileThanInMemory) {
    expect_refused({{0x0, 0x0, 0x200, 0x100, PF_R | PF_W}}, {}, Status::InvArgs);
}

TEST(PlanMappings, RefusesWritableAndExecutable) {
    expect_refused({{0x0, 0x0, 0x10, 0x10, PF_R | PF_W | PF_X}}, {}, Status::InvArgs);
    expect_refused({{0x0, 0x0, 0x10, 0x10, PF_W | PF_X}}, {}, Status::InvArgs);
}

TEST(PlanMappings, RefusesNoAccessAtAll) {
    expect_refused({{0x0, 0x0, 0x10, 0x10, 0}}, {}, Status::InvArgs);
    // Bits that are not PF_R, PF_W or PF_X do not count as access.
    expect_refused({{0x0, 0x0, 0x10, 0x10, 0xf0000000}}, {}, Status::InvArgs);
}

TEST(PlanMappings, RefusesAddressAndOffsetThatDisagreeWithinAPage) {
    expect_refused({{0x1010, 0x2020, 0x10, 0x10, PF_R}}, {}, Status::InvArgs);
}

TEST(PlanMappings, RefusesBytesPastTheEndOfTheFile) {
    // The last byte is at 0x3fff: the segment ends exactly at the end of the
    // file and is fine, one byte more is not.
    EXPECT_TRUE(build({{0x3f00, 0x3f00, 0x100, 0x100, PF_R}}, {}).is_ok());
    expect_refused({{0x3f00, 0x3f00, 0x101, 0x101, PF_R}}, {}, Status::InvArgs);
}

TEST(PlanMappings, RefusesArithmeticThatOverflows) {
    constexpr auto MAX = std::numeric_limits<u64>::max();
    expect_refused({{MAX - 0x10, 0x0, 0x10, 0x100, PF_R | PF_W}}, {}, Status::InvArgs);
    expect_refused({{0x0, MAX - 0x10, 0x100, 0x100, PF_R}}, {}, Status::InvArgs, MAX);
    // vaddr + mem_size does not overflow; rounding it up to a page does.
    expect_refused({{MAX - 0xfff, 0x0, 0x10, 0x100, PF_R | PF_W}}, {}, Status::InvArgs);
}

TEST(PlanMappings, RefusesZeroFillOfAFileMapping) {
    expect_refused({{0x0, 0x0, 0x100, 0x200, PF_R}}, {}, Status::InvArgs);
    expect_refused({{0x0, 0x0, 0x100, 0x200, PF_R | PF_X}}, {}, Status::InvArgs);
}

TEST(PlanMappings, RefusesSegmentsOutOfOrder) {
    expect_refused({{0x2000, 0x2000, 0x10, 0x10, PF_R}, {0x0, 0x0, 0x10, 0x10, PF_R}}, {},
                   Status::InvArgs);
}

TEST(PlanMappings, RefusesSegmentsThatShareAPage) {
    // The first ends at 0x1900, the second starts at 0x1a00: the same page,
    // which one mapping of one protection cannot be for both.
    expect_refused({{0x0, 0x0, 0x1900, 0x1900, PF_R}, {0x1a00, 0x1a00, 0x100, 0x100, PF_R | PF_X}},
                   {}, Status::InvArgs);
    // The next page is fine.
    EXPECT_TRUE(
        build({{0x0, 0x0, 0x1900, 0x1900, PF_R}, {0x2000, 0x2000, 0x100, 0x100, PF_R | PF_X}}, {})
            .is_ok());
}

TEST(PlanMappings, RefusesRelroThatDoesNotStartTheSegment) {
    std::vector<Segment> const segments = {{0x3000, 0x2000, 0x2000, 0x2000, PF_R | PF_W}};
    expect_refused(segments, {0x3100, 0x100}, Status::InvArgs);
}

TEST(PlanMappings, RefusesRelroLongerThanItsSegment) {
    std::vector<Segment> const segments = {{0x3000, 0x2000, 0x1000, 0x1000, PF_R | PF_W}};
    expect_refused(segments, {0x3000, 0x1001}, Status::InvArgs);
}

TEST(PlanMappings, RefusesRelroInASegmentThatIsNotWritable) {
    expect_refused(TYPICAL, {0x2000, 0x90}, Status::InvArgs);
}

TEST(PlanMappings, RefusesRelroInNoSegment) {
    expect_refused(TYPICAL, {0x9000, 0x100}, Status::InvArgs);
}
} // namespace
