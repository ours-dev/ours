/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host unit test for the pure buddy allocator. Mirrors the kernel/lib/arch
/// smoke-test convention: a plain main() that returns a non-zero exit code on
/// the first failing check. No kernel is booted; PmFrame storage lives on the
/// host heap and the global MemMap is never touched, so each test knows the
/// pfn of every descriptor it frees and passes it in, as the zone does.
#include <ours/mm/priv/frame_buddy.hpp>

#include <array>
#include <memory>
#include <string_view>
#include <vector>

#include <cstdio>
#include <cstdlib>

// Minimal host definitions of the panic hooks declared by <ours/panic.hpp>.
// Only reached if an ASSERT actually fires; both must exist to satisfy the
// link. FrameBuddy::release() asserts on its arguments, and a failed ASSERT
// renders its message and hands it to the string_view overload.
namespace ours {
template <typename String, typename FormatArgs>
[[noreturn]]
auto vpanic(String, FormatArgs const &) -> void {
    std::fprintf(stderr, "PANIC\n");
    std::abort();
}

[[noreturn]]
auto vpanic(std::string_view message) -> void {
    std::fprintf(stderr, "PANIC: %.*s\n", static_cast<int>(message.size()), message.data());
    std::abort();
}
} // namespace ours

namespace {
using ours::mm::FrameBuddy;
using ours::mm::PageFrame;
using ours::mm::Pfn;
using ours::mm::PmFrame;
using ours::mm::ZoneType;

auto make_frames(usize n) -> std::unique_ptr<PmFrame[]> {
    auto frames = std::make_unique<PmFrame[]>(n);
    for (usize i = 0; i < n; ++i) {
        frames[i].init(ZoneType::Normal, 0, 0);
    }
    return frames;
}

/// The pfn of |frame| in |frames|, a test's one array that starts at |base|.
auto pfn_in(PmFrame const *frames, PmFrame const *frame, Pfn base) -> Pfn {
    return base + static_cast<Pfn>(frame - frames);
}

auto check(bool ok, char const *what) -> int {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        return 1;
    }
    return 0;
}

auto test_exhaust_and_coalesce() -> int {
    constexpr auto ORDER = 8;
    constexpr auto NUM_FRAMES = usize{1} << ORDER;

    auto frames = make_frames(NUM_FRAMES);
    FrameBuddy buddy(0, NUM_FRAMES);

    buddy.release(&frames[0], 0, ORDER);
    if (check(!buddy.empty(ORDER) && buddy.free_count(ORDER) == 1,
              "populate creates one order-8 block")) {
        return 1;
    }

    std::vector<PmFrame *> got;
    got.reserve(NUM_FRAMES);
    for (usize i = 0; i < NUM_FRAMES; ++i) {
        auto const frame = buddy.allocate(0);
        if (!frame) {
            std::fprintf(stderr, "FAILED: exhausted at frame %zu\n", i);
            return 1;
        }
        got.push_back(&frame.value());
    }
    if (check(buddy.allocate(0).is_none() && buddy.empty(ORDER), "no frames remain after drain")) {
        return 1;
    }

    for (auto *frame: got) {
        buddy.release(frame, pfn_in(frames.get(), frame, 0), 0);
    }
    return check(buddy.free_count(ORDER) == 1, "full coalesce back to one order-8 block");
}

auto test_upper_buddy_merge() -> int {
    auto frames = make_frames(2);
    FrameBuddy buddy(0, 2);
    buddy.release(&frames[0], 0, 1);

    auto const first = buddy.allocate(0);
    auto const second = buddy.allocate(0);
    if (check(first.is_some() && second.is_some(), "two order-0 allocations succeed")) {
        return 1;
    }

    auto *const a = &first.value();
    auto *const b = &second.value();
    buddy.release(b, pfn_in(frames.get(), b, 0), 0);
    buddy.release(a, pfn_in(frames.get(), a, 0), 0);
    return check(buddy.free_count(1) == 1, "upper-buddy release merges into order-1 block");
}

auto check_foreign_buddy(ZoneType zone, ours::NodeId nid, char const *what) -> int {
    auto frames = make_frames(2);
    frames[1].init(zone, 0, nid);
    FrameBuddy local(0, 2);
    FrameBuddy foreign(0, 2);
    foreign.release(&frames[1], 1, 0);
    local.release(&frames[0], 0, 0);

    auto const left = local.allocate(0);
    auto const right = foreign.allocate(0);
    return check(left && &left.value() == &frames[0] && right && &right.value() == &frames[1],
                 what);
}

auto test_different_orders() -> int {
    auto frames = make_frames(4);
    FrameBuddy buddy(0, 4);
    // PFN 1 is still in use, so PFN 0 cannot coalesce with the upper pair.
    buddy.release(&frames[0], 0, 0);
    buddy.release(&frames[2], 2, 1);
    return check(buddy.free_count(0) == 1 && buddy.free_count(1) == 1 && buddy.empty(2),
                 "a free partner of a different order cannot coalesce");
}

auto test_split_shapes() -> int {
    constexpr auto ORDER = 8;
    constexpr auto NUM_FRAMES = usize{1} << ORDER;

    auto frames = make_frames(NUM_FRAMES);
    FrameBuddy buddy(0, NUM_FRAMES);
    buddy.release(&frames[0], 0, ORDER);

    auto const allocated = buddy.allocate(3);
    if (check(allocated.is_some() && &allocated.value() == &frames[0],
              "allocate(3) returns the base frame")) {
        return 1;
    }
    return check(buddy.free_count(3) == 1 && buddy.free_count(4) == 1 && buddy.free_count(5) == 1 &&
                     buddy.free_count(6) == 1 && buddy.free_count(7) == 1,
                 "one free block at each intermediate order");
}

/// A span whose descriptors are two arrays allocated apart, one max-order
/// block each, as the sparse memmap gives a zone that spans two sections. The
/// span starts at a pfn other than 0 and the second array is allocated
/// first, so neither its address nor its pfn follows from the first array's.
/// Every frame of the second block is taken and given back one by one; the
/// block must come back whole, and must not merge with the first.
auto test_two_arrays() -> int {
    constexpr auto BLOCK = usize{1} << FrameBuddy::MAX_ORDER;
    constexpr auto START = Pfn{0x8000};

    auto upper = make_frames(BLOCK);
    auto lower = make_frames(BLOCK);
    FrameBuddy buddy(START, START + 2 * BLOCK);

    buddy.release(&upper[0], START + BLOCK, FrameBuddy::MAX_ORDER);
    std::vector<PmFrame *> got;
    got.reserve(BLOCK);
    for (usize i = 0; i < BLOCK; ++i) {
        auto const frame = buddy.allocate(0);
        if (check(frame.is_some(), "the second array's block yields every frame")) {
            return 1;
        }
        got.push_back(&frame.value());
    }
    for (auto *frame: got) {
        buddy.release(frame, pfn_in(upper.get(), frame, START + BLOCK), 0);
    }
    if (check(buddy.free_count(FrameBuddy::MAX_ORDER) == 1 && buddy.free_count(0) == 0,
              "frames freed one by one in the second array coalesce to one max-order block")) {
        return 1;
    }

    buddy.release(&lower[0], START, FrameBuddy::MAX_ORDER);
    return check(buddy.free_count(FrameBuddy::MAX_ORDER) == 2,
                 "two max-order blocks in two arrays stay two blocks");
}

/// A span whose ends are not aligned to a max-order block, as a zone's need
/// not be, over two arrays. Just outside each end lies a descriptor of the
/// same zone and node that is a free order-0 block of another buddy, as a
/// neighbouring zone's would be: as far as the descriptor can tell, the first
/// frame's and the last frame's partner. Only the span check keeps the merge
/// from unlinking it, which would take it off its own buddy's list. Every
/// frame of the span is freed one by one, then all are taken again: merges
/// and splits both reach the first and the last pfn.
auto test_span_edges() -> int {
    constexpr auto BLOCK = usize{1} << FrameBuddy::MAX_ORDER;
    constexpr auto BASE = Pfn{0x8000};
    constexpr auto START = BASE + 1;
    constexpr auto END = BASE + 2 * BLOCK - 3;

    auto lower = make_frames(BLOCK);
    auto upper = make_frames(BLOCK);
    auto const frame_of = [&](Pfn const pfn) -> PmFrame * {
        if (pfn < BASE + BLOCK) {
            return &lower[pfn - BASE];
        }
        return &upper[pfn - BASE - BLOCK];
    };

    // The two frames just outside the span are free in buddies of their own,
    // as a neighbouring zone's would be: Pmm, order 0, and not this buddy's.
    FrameBuddy below(START - 1, START);
    FrameBuddy above(END, END + 1);
    below.release(frame_of(START - 1), START - 1, 0);
    above.release(frame_of(END), END, 0);
    std::array<PmFrame *, 2> const outside{frame_of(START - 1), frame_of(END)};

    FrameBuddy buddy(START, END);
    for (auto pfn = START; pfn < END; ++pfn) {
        buddy.release(frame_of(pfn), pfn, 0);
    }

    // [BASE + 1, BASE + BLOCK) comes back as one block of each order from 0
    // to MAX_ORDER - 1; [BASE + BLOCK, END), BLOCK - 3 frames, as one of each
    // from MAX_ORDER - 1 down to 2, then one of order 0.
    constexpr std::array<usize, FrameBuddy::NUM_ORDERS> EXPECTED{2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0};
    for (usize order = 0; order < FrameBuddy::NUM_ORDERS; ++order) {
        if (check(buddy.free_count(order) == EXPECTED[order],
                  "a span with unaligned ends merges exactly as far as its ends")) {
            return 1;
        }
    }

    usize taken = 0;
    while (auto const frame = buddy.allocate(0)) {
        if (check(&frame.value() != outside[0] && &frame.value() != outside[1],
                  "a split never hands out a descriptor outside the span")) {
            return 1;
        }
        taken += 1;
    }
    if (check(taken == END - START, "every frame of the span is handed out once")) {
        return 1;
    }

    for (auto *frame: outside) {
        if (check(frame->role() == PageFrame::Role::Pmm && frame->is_linked(),
                  "the descriptors outside the span are untouched")) {
            return 1;
        }
    }
    return check(below.free_count(0) == 1 && above.free_count(0) == 1,
                 "the neighbouring buddies still hold their one frame each");
}
} // namespace

auto main() -> int {
    if (auto rc = test_exhaust_and_coalesce(); rc != 0) {
        return rc;
    }
    if (auto rc = test_upper_buddy_merge(); rc != 0) {
        return rc;
    }
    if (auto rc = check_foreign_buddy(ZoneType::Dma, 0, "buddies in different zones stay separate");
        rc != 0) {
        return rc;
    }
    if (auto rc =
            check_foreign_buddy(ZoneType::Normal, 1, "buddies on different nodes stay separate");
        rc != 0) {
        return rc;
    }
    if (auto rc = test_different_orders(); rc != 0) {
        return rc;
    }
    if (auto rc = test_split_shapes(); rc != 0) {
        return rc;
    }
    if (auto rc = test_two_arrays(); rc != 0) {
        return rc;
    }
    if (auto rc = test_span_edges(); rc != 0) {
        return rc;
    }
    std::printf("frame-buddy smoke: OK\n");
    return 0;
}
