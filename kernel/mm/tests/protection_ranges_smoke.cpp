/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host unit test for the protection-range tracker. Mirrors the kernel/mm
/// smoke-test convention: a plain main() that returns a non-zero exit code on
/// the first failing check. No kernel is booted; the tracker is pure
/// bookkeeping, so every behaviour of interest is observable from the host.
#include <ours/mm/protection_ranges.hpp>

#include <string_view>
#include <vector>

#include <cstdio>
#include <cstdlib>

// Minimal host definitions of the panic hooks declared by <ours/panic.hpp>.
// DEBUG_ASSERT only reaches the string_view overload (the form the debug
// kernel builds use); the template covers non-debug ASSERT signatures. Both
// must exist to satisfy the link.
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
using ours::mm::MmuFlags;
using ours::mm::ProtectionRanges;

constexpr auto const WINDOW_BASE = VirtAddr{0x0040'0000};
constexpr auto const PAGE = usize{0x1000};

constexpr auto RW = MmuFlags::Readable | MmuFlags::Writable;
constexpr auto R = MmuFlags::Readable;
constexpr auto RX = MmuFlags::Readable | MmuFlags::Executable;

struct Span {
    VirtAddr base;
    usize size;
    MmuFlags mmuf;
};

auto check(bool ok, char const *what) -> int {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        return 1;
    }
    return 0;
}

auto collect(ProtectionRanges const &ranges, VirtAddr base, usize size) -> std::vector<Span> {
    std::vector<Span> spans;
    auto enumerator = ranges.make_enumerator(base, size);
    while (auto [valid, span_base, span_size, mmuf] = enumerator.next()) {
        spans.push_back(Span{span_base, span_size, mmuf});
    }
    return spans;
}

/// Compare what the enumerator produced against the expected tiling; both
/// vectors are in address order.
auto check_spans(std::vector<Span> const &got, std::vector<Span> const &want, char const *what)
    -> int {
    if (got.size() != want.size()) {
        std::fprintf(stderr, "FAILED: %s (got %zu spans, want %zu)\n", what, got.size(),
                     want.size());
        return 1;
    }
    for (usize i = 0; i < got.size(); ++i) {
        if (got[i].base != want[i].base || got[i].size != want[i].size ||
            got[i].mmuf != want[i].mmuf) {
            std::fprintf(stderr, "FAILED: %s (span %zu mismatch)\n", what, i);
            return 1;
        }
    }
    return 0;
}

auto test_init_and_full_window() -> int {
    ProtectionRanges ranges;
    ranges.validate();

    if (check(ranges.init(WINDOW_BASE, 16 * PAGE, RW) == Status::Ok, "init succeeds")) {
        return 1;
    }
    ranges.validate();

    if (check(ranges.init(WINDOW_BASE, 0, RW) == Status::InvalidArguments,
              "init rejects a zero-sized window")) {
        return 1;
    }

    return check_spans(collect(ranges, WINDOW_BASE, 16 * PAGE), {{WINDOW_BASE, 16 * PAGE, RW}},
                       "init lays one region over the window");
}

auto test_middle_update_splits_in_three() -> int {
    ProtectionRanges ranges;
    (void)ranges.init(WINDOW_BASE, 16 * PAGE, RW);

    auto const mid_base = WINDOW_BASE + 4 * PAGE;
    if (check(ranges.update(mid_base, 4 * PAGE, RX) == Status::Ok, "middle update succeeds")) {
        return 1;
    }
    ranges.validate();

    int const failed = check_spans(collect(ranges, WINDOW_BASE, 16 * PAGE),
                                   {{WINDOW_BASE, 4 * PAGE, RW},
                                    {mid_base, 4 * PAGE, RX},
                                    {mid_base + 4 * PAGE, 8 * PAGE, RW}},
                                   "middle update tiles the window in three");
    if (failed) {
        return failed;
    }

    // An enumerator clipped into the middle of the RX region sees only the
    // intersection.
    return check_spans(collect(ranges, mid_base + PAGE, 2 * PAGE),
                       {{mid_base + PAGE, 2 * PAGE, RX}},
                       "enumerator clips to the requested range");
}

auto test_restore_coalesces_back() -> int {
    ProtectionRanges ranges;
    (void)ranges.init(WINDOW_BASE, 16 * PAGE, RW);
    auto const mid_base = WINDOW_BASE + 4 * PAGE;
    (void)ranges.update(mid_base, 4 * PAGE, RX);

    if (check(ranges.update(mid_base, 4 * PAGE, RW) == Status::Ok, "restore succeeds")) {
        return 1;
    }
    ranges.validate();

    // Restoring the previous flags must merge the three regions back into one,
    // otherwise repeated protect/unprotect cycles would grow the list forever.
    return check_spans(collect(ranges, WINDOW_BASE, 16 * PAGE), {{WINDOW_BASE, 16 * PAGE, RW}},
                       "restoring flags coalesces back into one region");
}

auto test_regime_widening_keeps_boundaries_tight() -> int {
    ProtectionRanges ranges;
    (void)ranges.init(WINDOW_BASE, 16 * PAGE, RW);
    auto const mid_base = WINDOW_BASE + 4 * PAGE;
    (void)ranges.update(mid_base, 4 * PAGE, RX);
    (void)ranges.update(WINDOW_BASE + 2 * PAGE, 8 * PAGE, RX);

    ranges.validate();
    return check_spans(collect(ranges, WINDOW_BASE, 16 * PAGE),
                       {{WINDOW_BASE, 2 * PAGE, RW},
                        {WINDOW_BASE + 2 * PAGE, 8 * PAGE, RX},
                        {WINDOW_BASE + 10 * PAGE, 6 * PAGE, RW}},
                       "an update that widens an existing regime keeps boundaries tight");
}

auto test_range_inside_one_region_splits_both_ends() -> int {
    ProtectionRanges ranges;
    (void)ranges.init(WINDOW_BASE, 16 * PAGE, RW);

    // Both endpoints land inside the single RW region, so the update must
    // detach a tail on each side and leave both carrying the old flags.
    auto const inner_base = WINDOW_BASE + 6 * PAGE;
    if (check(ranges.update(inner_base, 2 * PAGE, R) == Status::Ok, "inner update succeeds")) {
        return 1;
    }
    ranges.validate();

    return check_spans(collect(ranges, WINDOW_BASE, 16 * PAGE),
                       {{WINDOW_BASE, 6 * PAGE, RW},
                        {inner_base, 2 * PAGE, R},
                        {inner_base + 2 * PAGE, 8 * PAGE, RW}},
                       "an update inside one region splits off both tails");
}

auto test_zero_permission_round_trip() -> int {
    ProtectionRanges ranges;
    (void)ranges.init(WINDOW_BASE, 16 * PAGE, RW);
    auto const mid_base = WINDOW_BASE + 4 * PAGE;

    // The "unmapped" convention: unmap() marks its range permission-less, and
    // a protect-style walk treats the zero-permission region accordingly.
    if (check(ranges.update(mid_base, 4 * PAGE, MmuFlags{}) == Status::Ok,
              "zero-permission update succeeds")) {
        return 1;
    }
    ranges.validate();

    auto spans = collect(ranges, WINDOW_BASE, 16 * PAGE);
    int const failed = check_spans(spans,
                                   {{WINDOW_BASE, 4 * PAGE, RW},
                                    {mid_base, 4 * PAGE, MmuFlags{}},
                                    {mid_base + 4 * PAGE, 8 * PAGE, RW}},
                                   "unmapped range becomes a zero-permission region");
    if (failed) {
        return failed;
    }

    if (check(!!(spans[1].mmuf == MmuFlags{}), "zero-permission region has no perms")) {
        return 1;
    }

    // And mapping it again coalesces the window back together.
    (void)ranges.update(mid_base, 4 * PAGE, RW);
    ranges.validate();
    return check_spans(collect(ranges, WINDOW_BASE, 16 * PAGE), {{WINDOW_BASE, 16 * PAGE, RW}},
                       "re-mapping coalesces the window back into one region");
}

auto test_edge_aligned_updates() -> int {
    ProtectionRanges ranges;
    (void)ranges.init(WINDOW_BASE, 16 * PAGE, RW);

    // Updates whose bounds coincide with existing region edges must not split
    // anything: carve an RX regime, then overwrite its lower quarter with the
    // original flags, leaving exactly two regions.
    (void)ranges.update(WINDOW_BASE + 8 * PAGE, 8 * PAGE, RX);
    (void)ranges.update(WINDOW_BASE + 8 * PAGE, 4 * PAGE, RW);
    ranges.validate();

    return check_spans(collect(ranges, WINDOW_BASE, 16 * PAGE),
                       {{WINDOW_BASE, 12 * PAGE, RW}, {WINDOW_BASE + 12 * PAGE, 4 * PAGE, RX}},
                       "edge-aligned updates keep the tiling minimal");
}

auto test_invalid_updates_are_rejected() -> int {
    ProtectionRanges ranges;
    (void)ranges.init(WINDOW_BASE, 16 * PAGE, RW);

    if (check(ranges.update(WINDOW_BASE, 0, RX) == Status::InvalidArguments,
              "zero-sized update rejected")) {
        return 1;
    }
    if (check(ranges.update(WINDOW_BASE - PAGE, PAGE, RX) == Status::InvalidArguments,
              "update below the window rejected")) {
        return 1;
    }
    if (check(ranges.update(WINDOW_BASE + 15 * PAGE, 2 * PAGE, RX) == Status::InvalidArguments,
              "update past the window rejected")) {
        return 1;
    }

    ranges.validate();
    return check_spans(collect(ranges, WINDOW_BASE, 16 * PAGE), {{WINDOW_BASE, 16 * PAGE, RW}},
                       "rejected updates leave the tiling untouched");
}

auto test_repeated_cycles_stay_flat() -> int {
    ProtectionRanges ranges;
    (void)ranges.init(WINDOW_BASE, 16 * PAGE, RW);

    // Flip one page's regime back and forth many times; coalescing must keep
    // the representation at three regions, not grow with the history.
    auto const page = WINDOW_BASE + 8 * PAGE;
    for (usize i = 0; i < 100; ++i) {
        auto const status = ranges.update(page, PAGE, (i % 2) ? RX : RW);
        if (Status::Ok != status) {
            std::fprintf(stderr, "FAILED: cycle %zu update\n", i);
            return 1;
        }
        ranges.validate();
    }

    return check_spans(collect(ranges, WINDOW_BASE, 16 * PAGE),
                       {{WINDOW_BASE, 8 * PAGE, RW}, {page, PAGE, RX}, {page + PAGE, 7 * PAGE, RW}},
                       "repeated flip-flop cycles keep the tiling flat");
}

} // namespace

auto main() -> int {
    int failed = 0;
    failed += test_init_and_full_window();
    failed += test_middle_update_splits_in_three();
    failed += test_restore_coalesces_back();
    failed += test_regime_widening_keeps_boundaries_tight();
    failed += test_range_inside_one_region_splits_both_ends();
    failed += test_zero_permission_round_trip();
    failed += test_edge_aligned_updates();
    failed += test_invalid_updates_are_rejected();
    failed += test_repeated_cycles_stay_flat();

    if (failed) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("protection-ranges-smoke: all tests passed\n");
    return 0;
}
