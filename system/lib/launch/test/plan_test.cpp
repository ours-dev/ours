/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// plan_mappings(): how the PT_LOAD segments of a program are mapped. The
/// specification is the comment on its definition, in src/plan.cpp.
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

/// A plan that starts out full of something else, so that a function which
/// only appends to it, or leaves a field it had nothing to say about, is caught.
auto dirty() -> Plan {
    Plan plan;
    for (auto &m: plan.mappings) {
        m = Mapping{Source::Private, 99, 0xdead000, 0xbeef000, 0xf00d000, Perm::RW};
    }
    plan.mapping_count = 5;
    for (auto &c: plan.copies) {
        c = Copy{99, 1, 2, 3};
    }
    plan.copy_count = 3;
    for (auto &s: plan.private_size) {
        s = 0x123000;
    }
    return plan;
}

auto plan_of(std::vector<Segment> const &segments, Relro const relro, Plan &plan,
             u64 const file_size = FILE_SIZE) -> Status {
    plan = dirty();
    return launch::plan_mappings(segments, relro, file_size, plan);
}

void expect_empty(Plan const &plan) {
    EXPECT_EQ(plan.mapping_count, 0U);
    EXPECT_EQ(plan.copy_count, 0U);
    for (auto const size: plan.private_size) {
        EXPECT_EQ(size, 0U);
    }
}

void expect_mapping(Plan const &plan, usize const index, Source const source, u32 const segment,
                    u64 const vaddr, u64 const size, u64 const vmo_offset, Perm const perm) {
    ASSERT_LT(index, plan.mapping_count);
    auto const &m = plan.mappings[index];
    EXPECT_EQ(m.source, source) << "mapping " << index;
    EXPECT_EQ(m.segment, segment) << "mapping " << index;
    EXPECT_EQ(m.vaddr, vaddr) << "mapping " << index;
    EXPECT_EQ(m.size, size) << "mapping " << index;
    EXPECT_EQ(m.vmo_offset, vmo_offset) << "mapping " << index;
    EXPECT_EQ(m.perm, perm) << "mapping " << index;
}

void expect_copy(Plan const &plan, usize const index, u32 const segment, u64 const vmo_offset,
                 u64 const file_offset, u64 const size) {
    ASSERT_LT(index, plan.copy_count);
    auto const &c = plan.copies[index];
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
    Plan plan;
    ASSERT_EQ(plan_of(TYPICAL, TYPICAL_RELRO, plan), Status::Ok);

    // The three segments that are not writable come from the file, a page each.
    ASSERT_EQ(plan.mapping_count, 5U);
    expect_mapping(plan, 0, Source::File, 0, 0x0000, 0x1000, 0x0000, Perm::R);
    expect_mapping(plan, 1, Source::File, 1, 0x1000, 0x1000, 0x1000, Perm::RX);
    expect_mapping(plan, 2, Source::File, 2, 0x2000, 0x1000, 0x2000, Perm::R);

    // The writable one is copied. Its first page is 0x3000, of which only the
    // last 0x100 bytes are RELRO -- and they are the whole of what is in that
    // page, so the page is read-only once relocated; the rest, .data and .bss,
    // stays writable.
    expect_mapping(plan, 3, Source::Private, 3, 0x3000, 0x1000, 0x0000, Perm::R);
    expect_mapping(plan, 4, Source::Private, 3, 0x4000, 0x3000, 0x1000, Perm::RW);

    ASSERT_EQ(plan.copy_count, 1U);
    expect_copy(plan, 0, 3, 0xf00, 0x2f00, 0x180);

    EXPECT_EQ(plan.private_size[0], 0U);
    EXPECT_EQ(plan.private_size[1], 0U);
    EXPECT_EQ(plan.private_size[2], 0U);
    EXPECT_EQ(plan.private_size[3], 0x4000U);
}

TEST(PlanMappings, ResetsWhatWasThere) {
    // The plan handed in was full, and the answer is exactly five mappings and
    // one copy: not five over the top of a leftover fifth, and not a stale
    // private_size on a segment that has none.
    Plan plan;
    ASSERT_EQ(plan_of({{0x0, 0x0, 0x10, 0x10, PF_R}}, {}, plan), Status::Ok);
    EXPECT_EQ(plan.mapping_count, 1U);
    EXPECT_EQ(plan.copy_count, 0U);
    for (auto const size: plan.private_size) {
        EXPECT_EQ(size, 0U);
    }
}

TEST(PlanMappings, NoRelroIsOneWritableMapping) {
    Plan plan;
    ASSERT_EQ(plan_of(TYPICAL, {}, plan), Status::Ok);
    ASSERT_EQ(plan.mapping_count, 4U);
    expect_mapping(plan, 3, Source::Private, 3, 0x3000, 0x4000, 0x0000, Perm::RW);
}

TEST(PlanMappings, RelroThatIsNotAWholePageIsNotProtected) {
    // 0x800 bytes of RELRO at the start of a page: the page also holds what
    // follows, which is written to, so it cannot be read-only.
    std::vector<Segment> const segments = {{0x3000, 0x2000, 0x1000, 0x1000, PF_R | PF_W}};
    Plan plan;
    ASSERT_EQ(plan_of(segments, {0x3000, 0x800}, plan), Status::Ok);
    ASSERT_EQ(plan.mapping_count, 1U);
    expect_mapping(plan, 0, Source::Private, 0, 0x3000, 0x1000, 0x0000, Perm::RW);
}

TEST(PlanMappings, RelroThatIsTheWholeSegmentLeavesNothingWritable) {
    std::vector<Segment> const segments = {{0x3000, 0x2000, 0x2000, 0x2000, PF_R | PF_W}};
    Plan plan;
    ASSERT_EQ(plan_of(segments, {0x3000, 0x2000}, plan), Status::Ok);
    ASSERT_EQ(plan.mapping_count, 1U);
    expect_mapping(plan, 0, Source::Private, 0, 0x3000, 0x2000, 0x0000, Perm::R);
}

TEST(PlanMappings, RelroSpanningPagesRoundsItsEndDown) {
    // RELRO from the start of the segment to 0x3a00 (offset 0x2a00): two whole
    // pages, then a page it only starts to fill.
    std::vector<Segment> const segments = {{0x1000, 0x0000, 0x3000, 0x3000, PF_R | PF_W}};
    Plan plan;
    ASSERT_EQ(plan_of(segments, {0x1000, 0x2a00}, plan), Status::Ok);
    ASSERT_EQ(plan.mapping_count, 2U);
    expect_mapping(plan, 0, Source::Private, 0, 0x1000, 0x2000, 0x0000, Perm::R);
    expect_mapping(plan, 1, Source::Private, 0, 0x3000, 0x1000, 0x2000, Perm::RW);
}

TEST(PlanMappings, ProtectionFollowsTheFlags) {
    Plan plan;
    // Execute without read is still readable when mapped, and write without
    // read is still a writable mapping.
    ASSERT_EQ(plan_of({{0x0, 0x0, 0x10, 0x10, PF_X}}, {}, plan), Status::Ok);
    expect_mapping(plan, 0, Source::File, 0, 0x0, 0x1000, 0x0, Perm::RX);
    ASSERT_EQ(plan_of({{0x0, 0x0, 0x10, 0x10, PF_W}}, {}, plan), Status::Ok);
    expect_mapping(plan, 0, Source::Private, 0, 0x0, 0x1000, 0x0, Perm::RW);
}

TEST(PlanMappings, WritableSegmentWithNothingInTheFileIsAllZero) {
    // A pure .bss: a private VMO, mapped, and nothing to copy into it.
    Plan plan;
    ASSERT_EQ(plan_of({{0x5000, 0x0, 0x0, 0x1800, PF_R | PF_W}}, {}, plan), Status::Ok);
    EXPECT_EQ(plan.copy_count, 0U);
    EXPECT_EQ(plan.private_size[0], 0x2000U);
    ASSERT_EQ(plan.mapping_count, 1U);
    expect_mapping(plan, 0, Source::Private, 0, 0x5000, 0x2000, 0x0, Perm::RW);
}

TEST(PlanMappings, SegmentThatStartsMidPageIsMappedFromItsPage) {
    // A read-only segment at 0x1234 in the file and in memory: the page it is
    // in is mapped, from the start of that page of the file.
    Plan plan;
    ASSERT_EQ(plan_of({{0x1234, 0x1234, 0x100, 0x100, PF_R}}, {}, plan), Status::Ok);
    ASSERT_EQ(plan.mapping_count, 1U);
    expect_mapping(plan, 0, Source::File, 0, 0x1000, 0x1000, 0x1000, Perm::R);
}

TEST(PlanMappings, SegmentThatRunsIntoTheNextPage) {
    // 0x1f00 + 0x200 crosses a page boundary: two pages.
    Plan plan;
    ASSERT_EQ(plan_of({{0x1f00, 0x1f00, 0x200, 0x200, PF_R}}, {}, plan), Status::Ok);
    ASSERT_EQ(plan.mapping_count, 1U);
    expect_mapping(plan, 0, Source::File, 0, 0x1000, 0x2000, 0x1000, Perm::R);
}

TEST(PlanMappings, TwoWritableSegmentsEachHaveTheirOwnVmo) {
    std::vector<Segment> const segments = {
        {0x1000, 0x2000, 0x10, 0x10, PF_R | PF_W},
        {0x3000, 0x3000, 0x20, 0x3000, PF_R | PF_W},
    };
    Plan plan;
    ASSERT_EQ(plan_of(segments, {}, plan), Status::Ok);
    ASSERT_EQ(plan.mapping_count, 2U);
    expect_mapping(plan, 0, Source::Private, 0, 0x1000, 0x1000, 0x0, Perm::RW);
    expect_mapping(plan, 1, Source::Private, 1, 0x3000, 0x3000, 0x0, Perm::RW);
    ASSERT_EQ(plan.copy_count, 2U);
    expect_copy(plan, 0, 0, 0x0, 0x2000, 0x10);
    expect_copy(plan, 1, 1, 0x0, 0x3000, 0x20);
    EXPECT_EQ(plan.private_size[0], 0x1000U);
    EXPECT_EQ(plan.private_size[1], 0x3000U);
}

TEST(PlanMappings, RelroBelongsToTheSegmentItStartsIn) {
    // Two writable segments; RELRO is the start of the second. The first is
    // untouched by it.
    std::vector<Segment> const segments = {
        {0x1000, 0x1000, 0x1000, 0x1000, PF_R | PF_W},
        {0x3000, 0x2000, 0x1000, 0x1000, PF_R | PF_W},
    };
    Plan plan;
    ASSERT_EQ(plan_of(segments, {0x3000, 0x1000}, plan), Status::Ok);
    ASSERT_EQ(plan.mapping_count, 2U);
    expect_mapping(plan, 0, Source::Private, 0, 0x1000, 0x1000, 0x0, Perm::RW);
    expect_mapping(plan, 1, Source::Private, 1, 0x3000, 0x1000, 0x0, Perm::R);
}

TEST(PlanMappings, MaximumSegments) {
    std::vector<Segment> segments;
    for (u64 i = 0; i < Plan::MAX_SEGMENTS; ++i) {
        segments.push_back({i * 0x1000, i * 0x1000, 0x10, 0x10, PF_R});
    }
    Plan plan;
    ASSERT_EQ(plan_of(segments, {}, plan, Plan::MAX_SEGMENTS * 0x1000), Status::Ok);
    EXPECT_EQ(plan.mapping_count, Plan::MAX_SEGMENTS);
}

// ---- What is refused. Every one leaves the plan empty.

void expect_refused(std::vector<Segment> const &segments, Relro const relro, Status const status,
                    u64 const file_size = FILE_SIZE) {
    Plan plan;
    EXPECT_EQ(plan_of(segments, relro, plan, file_size), status);
    expect_empty(plan);
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
    Plan plan;
    EXPECT_EQ(plan_of({{0x3f00, 0x3f00, 0x100, 0x100, PF_R}}, {}, plan), Status::Ok);
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
    Plan plan;
    EXPECT_EQ(
        plan_of({{0x0, 0x0, 0x1900, 0x1900, PF_R}, {0x2000, 0x2000, 0x100, 0x100, PF_R | PF_X}}, {},
                plan),
        Status::Ok);
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
