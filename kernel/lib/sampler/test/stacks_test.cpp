/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Host-side coverage for how a stack becomes addresses: the kernel unwinder's
// verdict on where a stack ends, the window that bounds its reads, and the
// frame-pointer walk a user stack takes. Every input is handed in -- a frame,
// tables, a reader over a fake stack -- so nothing here needs a kernel.
#include <sampler/format.hpp>
#include <sampler/frame_pointers.hpp>
#include <sampler/kernel_unwinder.hpp>
#include <sampler/stack_window.hpp>

#include <array>
#include <map>

#include <gtest/gtest.h>

namespace {

/// A stack in a std::map, as the frame-pointer walk's reader sees it:
/// addresses not in the map are not there.
struct FakeStack final {
    std::map<u64, u64> words;
};

auto read_fake_stack(void *context, u64 address, u64 *out) -> bool {
    auto const &stack = static_cast<FakeStack const *>(context)->words;
    auto const it = stack.find(address);
    if (it == stack.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

TEST(FrameWalk, FollowsTheChainOutwards) {
    // Three frames: 0x1000 -> 0x1020 -> 0x1040, the last one's caller is 0.
    FakeStack stack;
    stack.words = {
        {0x1000, 0x1020}, {0x1008, 0xa1}, // frame 1: next fp, return address
        {0x1020, 0x1040}, {0x1028, 0xa2}, // frame 2
        {0x1040, 0x0000}, {0x1048, 0x0},  // the outermost: no return address
    };
    u64 ips[8] = {};
    auto const depth =
        sampler::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips));
    ASSERT_EQ(depth, 3u);
    EXPECT_EQ(ips[0], 0xa0u);
    EXPECT_EQ(ips[1], 0xa1u);
    EXPECT_EQ(ips[2], 0xa2u);
}

TEST(FrameWalk, StopsAtMemoryThatIsNotThere) {
    FakeStack stack;
    stack.words = {{0x1000, 0x2000}, {0x1008, 0xa1}};
    u64 ips[8] = {};
    EXPECT_EQ(
        sampler::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips)),
        2u);
}

TEST(FrameWalk, StopsAtAChainThatDoesNotMoveUp) {
    // A frame that points at itself would loop for ever.
    FakeStack stack;
    stack.words = {{0x1000, 0x1000}, {0x1008, 0xa1}};
    u64 ips[8] = {};
    EXPECT_EQ(
        sampler::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips)),
        2u);
}

TEST(FrameWalk, RefusesAMisalignedOrNullFramePointer) {
    FakeStack stack;
    u64 ips[8] = {};
    EXPECT_EQ(sampler::walk_frame_pointers(0xa0, 0x1003, {&stack, &read_fake_stack}, ips, 8), 1u);
    EXPECT_EQ(sampler::walk_frame_pointers(0xa0, 0, {&stack, &read_fake_stack}, ips, 8), 1u);
}

TEST(FrameWalk, KeepsAtMostTheRoomGiven) {
    FakeStack stack;
    for (u64 frame = 0; frame < 8; ++frame) {
        stack.words[0x1000 + frame * 0x20] = 0x1000 + (frame + 1) * 0x20;
        stack.words[0x1008 + frame * 0x20] = 0xb0 + frame;
    }
    u64 ips[3] = {};
    EXPECT_EQ(
        sampler::walk_frame_pointers(0xa0, 0x1000, {&stack, &read_fake_stack}, ips, std::size(ips)),
        3u);
}

TEST(StackWindow, IsEmptyUnlessTheStackPointerIsOnTheStack) {
    constexpr u64 BASE = 0x7000'0000;
    constexpr u64 TOP = BASE + 0x4000;
    EXPECT_FALSE(sampler::StackWindow::on_stack(TOP - 0x100, BASE, TOP).empty());
    EXPECT_TRUE(sampler::StackWindow::on_stack(BASE - 8, BASE, TOP).empty());
    EXPECT_TRUE(sampler::StackWindow::on_stack(TOP, BASE, TOP).empty()); // top is exclusive
    EXPECT_TRUE(sampler::StackWindow::on_stack(0x1000, 0, 0).empty());   // bounds unknown
    EXPECT_TRUE(sampler::StackWindow::on_stack(TOP - 8, TOP, BASE).empty());
}

TEST(StackWindow, ContainsWordsFromTheStackPointerToTheTop) {
    constexpr u64 BASE = 0x7000'0000;
    constexpr u64 TOP = BASE + 0x4000;
    constexpr u64 SP = TOP - 0x100;
    auto const window = sampler::StackWindow::on_stack(SP, BASE, TOP);

    EXPECT_TRUE(window.contains(SP));
    EXPECT_TRUE(window.contains(TOP - 8)); // the last whole word
    EXPECT_FALSE(window.contains(TOP));    // past the top
    EXPECT_FALSE(window.contains(SP - 8)); // below the stack pointer: not this frame's caller
    EXPECT_FALSE(window.contains(SP + 4)); // misaligned
    EXPECT_FALSE(window.contains(BASE));   // on the stack, but below sp
}

TEST(StackWindow, RefusesAWordThatWouldWrapPastTheTopOfTheAddressSpace) {
    constexpr u64 TOP = 0xFFFF'FFFF'FFFF'F000;
    constexpr u64 BASE = TOP - 0x4000;
    auto const window = sampler::StackWindow::on_stack(TOP - 0x100, BASE, TOP);
    // address + 8 wraps to 0 here; a check written that way would pass it.
    EXPECT_FALSE(window.contains(0xFFFF'FFFF'FFFF'FFF8));
    EXPECT_TRUE(window.contains(TOP - 8));
}

TEST(StackWindow, AWindowNarrowerThanAWordContainsNothing) {
    constexpr u64 BASE = 0x7000'0000;
    constexpr u64 TOP = BASE + 0x4000;
    auto const window = sampler::StackWindow::on_stack(TOP - 4, BASE, TOP);
    EXPECT_FALSE(window.empty());
    EXPECT_FALSE(window.contains(TOP - 4));
    EXPECT_FALSE(window.contains(TOP - 8));
}

TEST(KernelUnwinder, AFrameWithNoUnwindInfoEndsTheStackOnlyAtARoot) {
    // No unwind tables at all, so every step finds no FDE: whether a stack is
    // whole depends only on where that frame is.
    constexpr u64 TOP = 0x8000'0000;
    constexpr u64 BASE = TOP - 0x4000;
    constexpr u64 ROOT = 0x5000;
    constexpr u64 ROOT_DEPTH = 176;
    sampler::StackRoot const roots[] = {{ROOT, ROOT_DEPTH}};
    sampler::KernelUnwinder unwinder;
    unwinder.init({.stack_roots = roots, .stack_root_count = 1});

    struct Case final {
        u64 pc;
        u64 sp;
        sampler::RecordFlags flags;
    };
    Case const cases[] = {
        {ROOT, TOP - ROOT_DEPTH, sampler::RecordFlags::None},           // the root itself
        {0x6000, TOP - ROOT_DEPTH, sampler::RecordFlags::Truncated},    // other code, same depth
        {ROOT, TOP - ROOT_DEPTH - 64, sampler::RecordFlags::Truncated}, // the stub, nested deeper
    };
    for (auto const &c: cases) {
        std::array<u64, sampler::MAX_STACK_DEPTH> ips{};
        auto const unwound = unwinder.unwind(
            {.pc = c.pc, .sp = c.sp, .bp = 0, .stack_base = BASE, .stack_top = TOP}, ips);
        EXPECT_EQ(unwound.depth, 1u) << c.pc;
        EXPECT_EQ(unwound.flags, c.flags) << c.pc;
        EXPECT_EQ(ips[0], c.pc);
    }
}

TEST(KernelUnwinder, AFrameOffItsStackKeepsItsPcAsAPrefix) {
    sampler::KernelUnwinder unwinder;
    unwinder.init({});
    std::array<u64, 4> ips{};
    // A kernel pc with no stack bounds given: nothing above it is read.
    auto const unwound =
        unwinder.unwind({.pc = 0x10, .sp = 0x1000, .bp = 0, .stack_base = 0, .stack_top = 0}, ips);
    EXPECT_EQ(unwound.depth, 1u);
    EXPECT_EQ(unwound.flags, sampler::RecordFlags::Truncated);
    EXPECT_EQ(ips[0], 0x10u);
}

} // namespace
