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

    // The writable one is copied. Its first page is 0x3000, of which only the
    // last 0x100 bytes are RELRO -- and they are the whole of what is in that
    // page, so the page is read-only once relocated; the rest, .data and .bss,
    // stays writable.
    expect_mapping(plan, 3, Source::Private, 3, 0x3000, 0x1000, 0x0000, Perm::R);
    expect_mapping(plan, 4, Source::Private, 3, 0x4000, 0x3000, 0x1000, Perm::RW);

    ASSERT_EQ(plan.copies().size(), 1U);
    expect_copy(plan, 0, 3, 0xf00, 0x2f00, 0x180);

    EXPECT_EQ(plan.private_size(0), 0U);
    EXPECT_EQ(plan.private_size(1), 0U);
    EXPECT_EQ(plan.private_size(2), 0U);
    EXPECT_EQ(plan.private_size(3), 0x4000U);
}

TEST(PlanMappings, NoRelroIsOneWritableMapping) {
    PLAN_OF(plan, TYPICAL, {});
    ASSERT_EQ(plan.mappings().size(), 4U);
    expect_mapping(plan, 3, Source::Private, 3, 0x3000, 0x4000, 0x0000, Perm::RW);
}

TEST(PlanMappings, RelroThatIsNotAWholePageIsNotProtected) {
    // 0x800 bytes of RELRO at the start of a page: the page also holds what
    // follows, which is written to, so it cannot be read-only.
    std::vector<Segment> const segments = {{0x3000, 0x2000, 0x1000, 0x1000, PF_R | PF_W}};
    PLAN_OF(plan, segments, {0x3000, 0x800});
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::Private, 0, 0x3000, 0x1000, 0x0000, Perm::RW);
}

TEST(PlanMappings, RelroThatIsTheWholeSegmentLeavesNothingWritable) {
    std::vector<Segment> const segments = {{0x3000, 0x2000, 0x2000, 0x2000, PF_R | PF_W}};
    PLAN_OF(plan, segments, {0x3000, 0x2000});
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::Private, 0, 0x3000, 0x2000, 0x0000, Perm::R);
}

TEST(PlanMappings, RelroSpanningPagesRoundsItsEndDown) {
    // RELRO from the start of the segment to 0x3a00 (offset 0x2a00): two whole
    // pages, then a page it only starts to fill.
    std::vector<Segment> const segments = {{0x1000, 0x0000, 0x3000, 0x3000, PF_R | PF_W}};
    PLAN_OF(plan, segments, {0x1000, 0x2a00});
    ASSERT_EQ(plan.mappings().size(), 2U);
    expect_mapping(plan, 0, Source::Private, 0, 0x1000, 0x2000, 0x0000, Perm::R);
    expect_mapping(plan, 1, Source::Private, 0, 0x3000, 0x1000, 0x2000, Perm::RW);
}

TEST(PlanMappings, ProtectionFollowsTheFlags) {
    // Execute without read is still readable when mapped, and write without
    // read is still a writable mapping.
    PLAN_OF(executable, {{0x0, 0x0, 0x10, 0x10, PF_X}}, {});
    expect_mapping(executable, 0, Source::File, 0, 0x0, 0x1000, 0x0, Perm::RX);
    PLAN_OF(writable, {{0x0, 0x0, 0x10, 0x10, PF_W}}, {});
    expect_mapping(writable, 0, Source::Private, 0, 0x0, 0x1000, 0x0, Perm::RW);
}

TEST(PlanMappings, WritableSegmentWithNothingInTheFileIsAllZero) {
    // A pure .bss: a private VMO, mapped, and nothing to copy into it.
    PLAN_OF(plan, {{0x5000, 0x0, 0x0, 0x1800, PF_R | PF_W}}, {});
    EXPECT_EQ(plan.copies().size(), 0U);
    EXPECT_EQ(plan.private_size(0), 0x2000U);
    ASSERT_EQ(plan.mappings().size(), 1U);
    expect_mapping(plan, 0, Source::Private, 0, 0x5000, 0x2000, 0x0, Perm::RW);
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
    ASSERT_EQ(plan.mappings().size(), 2U);
    expect_mapping(plan, 0, Source::Private, 0, 0x1000, 0x1000, 0x0, Perm::RW);
    expect_mapping(plan, 1, Source::Private, 1, 0x3000, 0x3000, 0x0, Perm::RW);
    ASSERT_EQ(plan.copies().size(), 2U);
    expect_copy(plan, 0, 0, 0x0, 0x2000, 0x10);
    expect_copy(plan, 1, 1, 0x0, 0x3000, 0x20);
    EXPECT_EQ(plan.private_size(0), 0x1000U);
    EXPECT_EQ(plan.private_size(1), 0x3000U);
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
    expect_mapping(plan, 0, Source::Private, 0, 0x1000, 0x1000, 0x0, Perm::RW);
    expect_mapping(plan, 1, Source::Private, 1, 0x3000, 0x1000, 0x0, Perm::R);
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
