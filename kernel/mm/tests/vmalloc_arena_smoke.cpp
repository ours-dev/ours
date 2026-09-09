/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host unit test for the vmalloc window's address allocator. Same convention
/// as protection_ranges_smoke.cpp beside it: a plain main() returning non-zero
/// on the first failing check, no kernel booted. The arena is pure address
/// bookkeeping -- it never touches a page table or a physical frame -- so
/// every behaviour of interest is reachable from the host, which is the whole
/// reason it is a separate class from mm::vmalloc().
#include <ours/mm/cfg.hpp>
#include <ours/mm/vmalloc_arena.hpp>

#include <string_view>
#include <vector>

#include <cstdio>
#include <cstdlib>

// Minimal host definitions of the panic hooks <ours/panic.hpp> declares, the
// same pair protection_ranges_smoke.cpp provides and for the same reason:
// DEBUG_ASSERT reaches the string_view overload in a debug build and the
// template in a release one, and both have to exist for the link.
namespace ours {
template <typename String, typename FormatArgs>
[[noreturn]]
auto vpanic(String, FormatArgs const &) -> void {
    std::fprintf(stderr, "PANIC\n");
    std::abort();
}

[[noreturn]]
auto vpanic(std::string_view) -> void {
    std::fprintf(stderr, "PANIC\n");
    std::abort();
}
} // namespace ours

namespace {
using ours::Status;
using ours::VirtAddr;
using ours::mm::VmallocArena;

constexpr auto WINDOW_BASE = VirtAddr{0x1000'0000};
constexpr auto WINDOW_PAGES = usize{64};
constexpr auto PAGE = usize{PAGE_SIZE};

auto check(bool const ok, char const *const what) -> int {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        return 1;
    }
    return 0;
}

auto make_arena() -> VmallocArena {
    VmallocArena arena;
    (void)arena.init(WINDOW_BASE, WINDOW_PAGES * PAGE);
    return arena;
}

auto test_init_validates_its_window() -> int {
    VmallocArena arena;
    int failed = 0;

    failed += check(arena.init(0, WINDOW_PAGES * PAGE) == Status::InvArgs, "init rejects base 0");
    failed += check(arena.init(WINDOW_BASE, 0) == Status::InvArgs, "init rejects an empty window");
    failed += check(arena.init(WINDOW_BASE + 1, WINDOW_PAGES * PAGE) == Status::InvArgs,
                    "init rejects a misaligned base");
    failed += check(arena.init(WINDOW_BASE, WINDOW_PAGES * PAGE + 1) == Status::InvArgs,
                    "init rejects a misaligned size");
    // A window whose end would wrap: every range check inside compares
    // base + size against the end, and a wrapped end inverts all of them.
    failed += check(arena.init(VirtAddr(-PAGE), 2 * PAGE) == Status::InvArgs,
                    "init rejects a window that wraps");

    failed += check(arena.init(WINDOW_BASE, WINDOW_PAGES * PAGE) == Status::Ok, "init succeeds");
    failed += check(arena.nr_holes() == 1, "a fresh window is one hole");
    failed += check(arena.nr_free_pages() == WINDOW_PAGES, "a fresh window is entirely free");
    return failed;
}

auto test_allocate_carves_from_the_front() -> int {
    auto arena = make_arena();
    int failed = 0;

    auto const first = arena.allocate(4);
    failed += check(first == WINDOW_BASE, "the first allocation starts at the window base");

    auto const second = arena.allocate(2);
    failed += check(second == WINDOW_BASE + 4 * PAGE, "the second follows the first");

    failed += check(arena.nr_holes() == 1, "carving the front leaves one hole");
    failed += check(arena.nr_free_pages() == WINDOW_PAGES - 6, "free pages fall by what was taken");
    failed += check(arena.nr_live() == 2, "two ranges are live");
    return failed;
}

auto test_allocate_refuses_what_does_not_fit() -> int {
    auto arena = make_arena();
    int failed = 0;

    failed += check(arena.allocate(0) == 0, "a zero-page request is refused");
    failed +=
        check(arena.allocate(WINDOW_PAGES + 1) == 0, "a request larger than the window is refused");
    failed += check(arena.nr_free_pages() == WINDOW_PAGES, "a refused request takes nothing");

    failed += check(arena.allocate(WINDOW_PAGES) == WINDOW_BASE, "the whole window can be taken");
    failed += check(arena.nr_holes() == 0, "taking the whole window leaves no hole");
    failed += check(arena.allocate(1) == 0, "an exhausted window refuses");
    return failed;
}

auto test_free_merges_both_ways() -> int {
    int failed = 0;

    // Three adjacent ranges, then free the outer two and finally the middle:
    // the last free has a neighbour on each side, which is the case that has
    // to collapse three holes into one rather than leave a seam.
    auto arena = make_arena();
    auto const a = arena.allocate(4);
    auto const b = arena.allocate(4);
    auto const c = arena.allocate(4);
    failed += check(a != 0 && b != 0 && c != 0, "three ranges allocated");

    // b and c stand between a and the tail hole, so returning a can merge with
    // nothing and has to become a hole of its own.
    failed += check(arena.free(a, 4) == Status::Ok, "freeing the first succeeds");
    failed += check(arena.nr_holes() == 2, "an isolated return becomes its own hole");

    failed += check(arena.free(c, 4) == Status::Ok, "freeing the third succeeds");
    // c is adjacent to the tail hole, so it merges rightwards rather than
    // adding one.
    failed += check(arena.nr_holes() == 2, "two holes with the middle still live");

    failed += check(arena.free(b, 4) == Status::Ok, "freeing the middle succeeds");
    failed += check(arena.nr_holes() == 1, "the middle collapses the three into one");
    failed += check(arena.nr_free_pages() == WINDOW_PAGES, "everything is back");
    failed += check(arena.nr_live() == 0, "no range is live");
    return failed;
}

auto test_free_merges_leftwards_only() -> int {
    auto arena = make_arena();
    int failed = 0;

    auto const a = arena.allocate(4);
    auto const b = arena.allocate(4);
    (void)arena.allocate(4); // kept live so b cannot merge rightwards

    failed += check(arena.free(a, 4) == Status::Ok, "freeing the first succeeds");
    failed += check(arena.free(b, 4) == Status::Ok, "freeing the second succeeds");
    failed += check(arena.nr_holes() == 2, "b merged into a rather than adding a hole");
    failed += check(arena.nr_free_pages() == WINDOW_PAGES - 4, "only the live range is missing");
    return failed;
}

auto test_free_validates_its_argument() -> int {
    auto arena = make_arena();
    int failed = 0;

    auto const a = arena.allocate(4);
    failed += check(arena.free(a, 0) == Status::InvArgs, "a zero-page return is refused");
    failed += check(arena.free(a + 1, 4) == Status::InvArgs, "a misaligned return is refused");
    failed += check(arena.free(WINDOW_BASE - PAGE, 1) == Status::InvArgs,
                    "a return below the window is refused");
    failed += check(arena.free(WINDOW_BASE + (WINDOW_PAGES - 1) * PAGE, 2) == Status::InvArgs,
                    "a return running past the window is refused");
    failed += check(arena.nr_free_pages() == WINDOW_PAGES - 4, "a refused return changes nothing");
    return failed;
}

/// The reuse pattern the boot self-check will run, and the one that would
/// expose a merge bug as slow leakage rather than as a crash: many rounds of
/// allocate-then-free must leave the window in exactly its initial state.
auto test_cycles_leave_no_residue() -> int {
    auto arena = make_arena();
    int failed = 0;

    for (auto round = usize{0}; round < 100; ++round) {
        std::vector<VirtAddr> live;
        for (auto n = usize{1}; n <= 5; ++n) {
            auto const address = arena.allocate(n);
            failed += check(address != 0, "allocation inside a cycle succeeds");
            live.push_back(address);
        }
        // Freed in the order that merges least eagerly -- interleaved, so the
        // hole count rises before it falls.
        for (auto n = usize{5}; n >= 1; --n) {
            failed +=
                check(arena.free(live[n - 1], n) == Status::Ok, "free inside a cycle succeeds");
        }
        if (failed) {
            return failed;
        }
    }

    failed += check(arena.nr_holes() == 1, "a hundred cycles leave one hole");
    failed +=
        check(arena.nr_free_pages() == WINDOW_PAGES, "a hundred cycles leave the window whole");
    failed += check(arena.base() == WINDOW_BASE, "the window did not move");
    return failed;
}

/// Fragmentation is bounded by MAX_HOLES, and the bound is enforced on the
/// allocating side so that free() can never fail. Take alternate pages until
/// the arena refuses, then check it refused for the documented reason and that
/// every outstanding range can still be returned.
auto test_live_count_is_capped_so_free_cannot_fail() -> int {
    // A window large enough that the hole cap, not the space, is what stops it.
    VmallocArena arena;
    (void)arena.init(WINDOW_BASE, 4 * VmallocArena::MAX_HOLES * PAGE);
    int failed = 0;

    std::vector<VirtAddr> live;
    while (auto const address = arena.allocate(1)) {
        live.push_back(address);
        if (live.size() > VmallocArena::MAX_HOLES) {
            break;
        }
    }

    // allocate() refuses once m_nr_live + 1 would reach MAX_HOLES, so the live
    // count settles at MAX_HOLES - 1. That is exactly the cap the geometric
    // bound needs: free holes and live ranges alternate along the window, so
    // holes <= live + 1 <= MAX_HOLES.
    failed +=
        check(live.size() == VmallocArena::MAX_HOLES - 1, "allocation stops at the live-range cap");
    failed += check(arena.nr_live() == live.size(), "the live count matches");

    // Every other one, which is the worst case for hole count: each free is
    // isolated and so adds a hole.
    for (usize i = 0; i < live.size(); i += 2) {
        failed += check(arena.free(live[i], 1) == Status::Ok, "an isolated free still succeeds");
    }
    failed += check(arena.nr_holes() <= VmallocArena::MAX_HOLES, "the hole array never overflows");

    for (usize i = 1; i < live.size(); i += 2) {
        failed += check(arena.free(live[i], 1) == Status::Ok, "the remaining frees succeed");
    }
    failed += check(arena.nr_holes() == 1, "returning everything coalesces back to one hole");
    failed += check(arena.nr_live() == 0, "nothing is live");
    return failed;
}

} // namespace

auto main() -> int {
    int failed = 0;
    failed += test_init_validates_its_window();
    failed += test_allocate_carves_from_the_front();
    failed += test_allocate_refuses_what_does_not_fit();
    failed += test_free_merges_both_ways();
    failed += test_free_merges_leftwards_only();
    failed += test_free_validates_its_argument();
    failed += test_cycles_leave_no_residue();
    failed += test_live_count_is_capped_so_free_cannot_fail();

    if (failed) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("vmalloc-arena-smoke: all tests passed\n");
    return 0;
}
