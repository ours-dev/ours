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
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <unwind/testing/eh_frame_builder.hpp>

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

TEST(StackWindow, AWindowNarrowerThanAWordAtTheBottomOfMemoryContainsNothing) {
    // Where the window's top is below 8, `hi - 8` wraps: without contains()'s
    // size check, the range test would pass address 0 and let an 8-byte read
    // run past the window's top. The window above is high in memory, where
    // the range test refuses both probes anyway.
    auto const window = sampler::StackWindow::on_stack(0, 0, 4);
    EXPECT_FALSE(window.empty());
    EXPECT_FALSE(window.contains(0));
}

/// Three functions with call frame information -- F called by G, G by H --
/// and a kernel stack in host memory to unwind them over. Each function runs
/// on the CIE's rules alone (no prologue: the caller's return address at the
/// stack pointer), which is enough to drive every branch of the unwind loop:
/// what the frames hold decides where the stack ends. The section pretends to
/// be loaded at 0, so the pcs are small numbers.
class UnwindHarness final {
  public:
    static constexpr u64 F_PC = 0x1000;
    static constexpr u64 G_PC = 0x2000;
    static constexpr u64 H_PC = 0x3000;
    /// A return address no FDE covers: assembly the unwinder cannot step
    /// through, or the entry stub that began the stack.
    static constexpr u64 NO_FDE_RET = 0x5010;
    /// Where in the stack F was interrupted, in words from its base.
    static constexpr usize SP = 8;

    UnwindHarness() {
        using namespace unwind::testing;
        m_eh.emit_cie();
        constexpr u8 CIE_RULES[] = {CFA_NOP};
        for (auto const pc: {F_PC, G_PC, H_PC}) {
            m_eh.emit_fde(pc, 0x100, CIE_RULES, sizeof(CIE_RULES));
        }
        usize count = 0;
        m_built = build_fde_entries(m_eh.section(), &m_entries, &count) && count == 3;
    }

    [[nodiscard]]
    auto built() const -> bool {
        return m_built;
    }

    /// The unwinder over these tables, with |roots| as the stack's roots.
    auto init(sampler::KernelUnwinder &unwinder,
              std::span<sampler::StackRoot const> const roots = {}) const -> void {
        unwinder.init({
            .eh_section = m_eh.section(),
            .fde_entries = m_entries.data(),
            .fde_entry_count = m_entries.size(),
            .stack_roots = roots.data(),
            .stack_root_count = roots.size(),
        });
    }

    /// Writes the word |index| words above F's stack pointer.
    auto above_sp(usize const index, u64 const value) -> void {
        m_stack[SP + index] = value;
    }

    /// The address of the word |index| words above F's stack pointer.
    [[nodiscard]]
    auto address_above_sp(usize const index) const -> u64 {
        return reinterpret_cast<u64>(&m_stack[SP + index]);
    }

    /// F, interrupted in its body.
    [[nodiscard]]
    auto frame() const -> sampler::KernelFrame {
        return {
            .pc = F_PC + 4,
            .sp = address_above_sp(0),
            .bp = 0,
            .stack_base = reinterpret_cast<u64>(m_stack.data()),
            .stack_top = reinterpret_cast<u64>(m_stack.data() + m_stack.size()),
        };
    }

  private:
    unwind::testing::FakeEhFrame m_eh;
    std::vector<unwind::FdeIndex::Entry> m_entries;
    /// What the tables' build answered: the fixture's one yes/no fact, an
    /// outcome to report rather than a state.
    bool m_built = false;
    alignas(16) std::array<u64, 32> m_stack{};
};

TEST(KernelUnwinder, WalksEveryFrameAndAZeroReturnEndsTheStackWhole) {
    UnwindHarness harness;
    ASSERT_TRUE(harness.built());
    harness.above_sp(0, UnwindHarness::G_PC + 0x10); // F returns into G
    harness.above_sp(1, UnwindHarness::H_PC + 0x10); // G returns into H
    harness.above_sp(2, 0);                          // H's caller: none, a thread's first frame
    sampler::KernelUnwinder unwinder;
    harness.init(unwinder);

    std::array<u64, sampler::MAX_STACK_DEPTH> ips{};
    auto const unwound = unwinder.unwind(harness.frame(), ips);
    ASSERT_EQ(unwound.depth, 3u);
    EXPECT_EQ(ips[0], UnwindHarness::F_PC + 4);
    EXPECT_EQ(ips[1], UnwindHarness::G_PC + 0x10);
    EXPECT_EQ(ips[2], UnwindHarness::H_PC + 0x10);
    // The end of a thread's stack is the whole stack, not a prefix.
    EXPECT_EQ(unwound.flags, sampler::RecordFlags::None);
}

TEST(KernelUnwinder, AFrameWithNoUnwindInfoIsAPrefixUnlessItIsARoot) {
    UnwindHarness harness;
    ASSERT_TRUE(harness.built());
    harness.above_sp(0, UnwindHarness::G_PC + 0x10);
    harness.above_sp(1, UnwindHarness::NO_FDE_RET); // G returns into code with no FDE
    std::array<u64, sampler::MAX_STACK_DEPTH> ips{};

    // Anywhere else, the stack goes on below a frame the unwinder cannot
    // step through.
    sampler::KernelUnwinder plain;
    harness.init(plain);
    auto const prefix = plain.unwind(harness.frame(), ips);
    ASSERT_EQ(prefix.depth, 3u);
    EXPECT_EQ(ips[2], UnwindHarness::NO_FDE_RET);
    EXPECT_EQ(prefix.flags, sampler::RecordFlags::Truncated);

    // At a root -- that return address, as deep below the stack's top as
    // the frame it was found in -- the stack began there.
    auto const top = harness.frame().stack_top;
    sampler::StackRoot const roots[] = {
        {UnwindHarness::NO_FDE_RET, top - harness.address_above_sp(2)}};
    sampler::KernelUnwinder rooted;
    harness.init(rooted, roots);
    auto const whole = rooted.unwind(harness.frame(), ips);
    ASSERT_EQ(whole.depth, 3u);
    EXPECT_EQ(whole.flags, sampler::RecordFlags::None);
}

TEST(KernelUnwinder, AStackDeeperThanTheRoomGivenIsAPrefix) {
    UnwindHarness harness;
    ASSERT_TRUE(harness.built());
    harness.above_sp(0, UnwindHarness::G_PC + 0x10);
    harness.above_sp(1, UnwindHarness::H_PC + 0x10);
    harness.above_sp(2, 0);
    sampler::KernelUnwinder unwinder;
    harness.init(unwinder);

    std::array<u64, 2> ips{};
    auto const unwound = unwinder.unwind(harness.frame(), ips);
    ASSERT_EQ(unwound.depth, 2u);
    EXPECT_EQ(ips[1], UnwindHarness::G_PC + 0x10);
    EXPECT_EQ(unwound.flags, sampler::RecordFlags::Truncated);
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
