/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host unit test for the pure buddy allocator. Mirrors the kernel/lib/arch
/// smoke-test convention: a plain main() that returns a non-zero exit code on
/// the first failing check. No kernel is booted; PmFrame storage lives on the
/// host heap and the global MemMap is never touched.
#include <ours/mm/priv/frame_buddy.hpp>

#include <memory>
#include <vector>

#include <cstdio>
#include <cstdlib>

// Minimal host definition of the panic hook declared by <ours/panic.hpp>.
// Only reached if an ASSERT actually fires; it must exist to satisfy the link.
namespace ours {
template <typename String, typename FormatArgs>
[[noreturn]]
auto vpanic(String, FormatArgs const &) -> void {
    std::fprintf(stderr, "PANIC\n");
    std::abort();
}
} // namespace ours

namespace {
using ours::mm::FrameBuddy;
using ours::mm::PmFrame;
using ours::mm::ZoneType;

auto make_frames(usize n) -> std::unique_ptr<PmFrame[]> {
    auto frames = std::make_unique<PmFrame[]>(n);
    for (usize i = 0; i < n; ++i) {
        frames[i].init(ZoneType::Normal, 0, 0);
    }
    return frames;
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
    FrameBuddy buddy(frames.get(), NUM_FRAMES, 0);

    buddy.release(&frames[0], ORDER);
    if (check(buddy.free_count(ORDER) == 1, "populate creates one order-8 block")) {
        return 1;
    }

    std::vector<PmFrame *> got;
    got.reserve(NUM_FRAMES);
    for (usize i = 0; i < NUM_FRAMES; ++i) {
        auto *frame = buddy.allocate(0);
        if (!frame) {
            std::fprintf(stderr, "FAILED: exhausted at frame %zu\n", i);
            return 1;
        }
        got.push_back(frame);
    }
    if (check(buddy.allocate(0) == nullptr, "no frames remain after drain")) {
        return 1;
    }

    for (auto *frame: got) {
        buddy.release(frame, 0);
    }
    return check(buddy.free_count(ORDER) == 1, "full coalesce back to one order-8 block");
}

auto test_upper_buddy_merge() -> int {
    auto frames = make_frames(2);
    FrameBuddy buddy(frames.get(), 2, 0);
    buddy.release(&frames[0], 1);

    auto *a = buddy.allocate(0);
    auto *b = buddy.allocate(0);
    if (check(a != nullptr && b != nullptr, "two order-0 allocations succeed")) {
        return 1;
    }

    buddy.release(b, 0);
    buddy.release(a, 0);
    return check(buddy.free_count(1) == 1, "upper-buddy release merges into order-1 block");
}

auto test_split_shapes() -> int {
    constexpr auto ORDER = 8;
    constexpr auto NUM_FRAMES = usize{1} << ORDER;

    auto frames = make_frames(NUM_FRAMES);
    FrameBuddy buddy(frames.get(), NUM_FRAMES, 0);
    buddy.release(&frames[0], ORDER);

    auto *allocated = buddy.allocate(3);
    if (check(allocated == &frames[0], "allocate(3) returns the base frame")) {
        return 1;
    }
    return check(buddy.free_count(3) == 1 && buddy.free_count(4) == 1 && buddy.free_count(5) == 1 &&
                     buddy.free_count(6) == 1 && buddy.free_count(7) == 1,
                 "one free block at each intermediate order");
}
} // namespace

auto main() -> int {
    if (auto rc = test_exhaust_and_coalesce(); rc != 0) {
        return rc;
    }
    if (auto rc = test_upper_buddy_merge(); rc != 0) {
        return rc;
    }
    if (auto rc = test_split_shapes(); rc != 0) {
        return rc;
    }
    std::printf("frame-buddy smoke: OK\n");
    return 0;
}
