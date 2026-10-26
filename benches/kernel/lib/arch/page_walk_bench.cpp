/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host benchmark for the paging engine's walks (arch::paging::Paging, x86
/// four-level), the measurement items 1 and 2 of
/// docs/repairs/261025/arch-paging.md ask for before anything changes.
///
/// Per operation it prints:
///
///   visits     how many times the engine entered a table: every
///              phys_to_table() call is one, and the PhysToVirt it is given
///              here counts them. Exact, and the same on any machine.
///   floor      how many distinct tables the range touches: what a walk that
///              went down to each table once, and then stepped through it,
///              would visit.
///   ns/page    host time per page. The engine's walks are its own software
///              work on ordinary memory, with no TLB in them, so the host's
///              number is a fair one for that work; it is not what the
///              kernel adds around it (the lock, interrupts off, the TLB
///              shootdown).
///
/// The tables live in one arena whose addresses stand in for physical ones
/// (identity phys_to_table). Leaf frames are never touched, so any address
/// will do for them. Deposits are counted, not kept: a reservation always
/// succeeds, and a withdrawal hands out a fresh table.
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arch/paging.hpp>
#include <arch/paging/x86_pagings.hpp>

namespace {
using namespace arch::paging;
using X86Paging = PagingDispatcher<X86PagingLevel::PageMapLevel4>::Type;
using Engine = arch::Paging<X86Paging>;
using ours::PhysAddr;
using ours::VirtAddr;

constexpr usize ARENA_TABLES = 1024;

/// The page tables, and the root at the start of them.
class Arena {
  public:
    Arena() {
        m_memory =
            static_cast<unsigned char *>(std::aligned_alloc(PAGE_SIZE, ARENA_TABLES * PAGE_SIZE));
        std::memset(m_memory, 0, ARENA_TABLES * PAGE_SIZE);
        m_next = 1;
    }
    Arena(Arena const &) = delete;
    auto operator=(Arena const &) -> Arena & = delete;
    ~Arena() {
        std::free(m_memory);
    }

    /// Empties every table handed out, the root included, for the next round.
    auto reset() -> void {
        std::memset(m_memory, 0, m_next * PAGE_SIZE);
        m_next = 1;
    }

    [[nodiscard]]
    auto root() const -> PhysAddr {
        return reinterpret_cast<PhysAddr>(m_memory);
    }

    auto allocate(usize const size, usize const alignment) -> PhysAddr {
        if (size != PAGE_SIZE || alignment != PAGE_SIZE || m_next == ARENA_TABLES) {
            return 0;
        }
        return reinterpret_cast<PhysAddr>(m_memory + (m_next++) * PAGE_SIZE);
    }

  private:
    unsigned char *m_memory;
    usize m_next;
};

/// Identity, counting every table the engine enters.
struct CountingPhysToVirt {
    usize *visits;

    auto operator()(PhysAddr const address) const -> VirtAddr {
        *visits += 1;
        return address;
    }
};

/// Deposits that are only counted: see the file comment.
struct CountedDeposits {
    Arena *arena;

    auto reserve(PhysAddr) -> bool {
        return true;
    }
    auto withdraw(PhysAddr) -> PhysAddr {
        return arena->allocate(PAGE_SIZE, PAGE_SIZE);
    }
    auto discard(PhysAddr) -> void {}
    auto discard_all(PhysAddr) -> void {}
};
static_assert(TableDeposits<CountedDeposits>);

constexpr auto USER_RW = MmuFlags::Present | MmuFlags::Writable | MmuFlags::User;
constexpr auto USER_RO = MmuFlags::Present | MmuFlags::User;
constexpr auto FRESH = MapOptions::ErrorIfExisting | MapOptions::UndoOnFailure;

/// Where the ranges start: 1 GiB, so that a range of up to 1 GiB stays under
/// one third-level entry, and the frames, 2 GiB, with |misalign| added.
constexpr VirtAddr BASE_VA = VirtAddr{1} << 30;
constexpr PhysAddr BASE_PA = PhysAddr{1} << 31;

/// How many distinct tables a range of |n| pages at |va| touches, with
/// final-level tables only when its leaves are 4 KiB.
auto floor_tables(VirtAddr const va, usize const n, bool const small) -> usize {
    auto const last = va + n * PAGE_SIZE - 1;
    auto const span = [&](unsigned const shift) {
        return static_cast<usize>((last >> shift) - (va >> shift) + 1);
    };
    // The root, one third-level table per root entry spanned, one second-level
    // table per third-level entry, and one final-level table per second-level
    // entry when the leaves are small.
    auto tables = 1 + span(39) + span(30);
    if (small) {
        tables += span(21);
    }
    return tables;
}

using Clock = std::chrono::steady_clock;

auto elapsed_ns(Clock::time_point const from, Clock::time_point const to) -> double {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
}

/// Pages each sample covers. The host clock ticks every 41.7 ns on Apple
/// silicon, so a sample has to last far longer than one operation on one
/// page: it maps, protects and unmaps |SAMPLE_PAGES / n| ranges of |n| pages
/// one after another, and is timed as a whole.
constexpr usize SAMPLE_PAGES = 32768;

/// Samples per case: the statistics' n.
constexpr usize SAMPLES = 32;

/// One raw sample: what was measured, what the number is, and the number.
struct Sample {
    std::string what;
    char const *metric;
    char const *unit;
    double value;
};

class Recorder {
  public:
    auto add(std::string const &what, char const *const metric, char const *const unit,
             double const value) -> void {
        m_samples.push_back({what, metric, unit, value});
    }

    /// The median of every sample of |what| and |metric|.
    [[nodiscard]]
    auto median(std::string const &what, char const *const metric) const -> double {
        std::vector<double> values;
        for (auto const &sample: m_samples) {
            if (sample.what == what && std::string(sample.metric) == metric) {
                values.push_back(sample.value);
            }
        }
        std::sort(values.begin(), values.end());
        if (values.empty()) {
            return 0;
        }
        auto const middle = values.size() / 2;
        if (values.size() % 2 == 1) {
            return values[middle];
        }
        return (values[middle - 1] + values[middle]) / 2;
    }

    /// One row per sample, in the format scripts/perf_stats.py reads.
    auto print_csv() const -> void {
        std::printf("case,metric,unit,value\n");
        for (auto const &sample: m_samples) {
            std::printf("%s,%s,%s,%.4f\n", sample.what.c_str(), sample.metric, sample.unit,
                        sample.value);
        }
    }

  private:
    std::vector<Sample> m_samples;
};

auto name(char const *const op, char const *const leaves, usize const n) -> std::string {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s %s %zu", op, leaves, n);
    return buffer;
}

/// |SAMPLES| samples of map, protect and unmap of ranges of |n| pages, with
/// the frames |misalign| off a 2 MiB boundary. Each sample starts from an
/// empty table. Gives time and table visits per page.
auto run(Recorder &recorder, char const *const leaves, usize const n, PhysAddr const misalign)
    -> void {
    Arena arena;
    CountedDeposits deposits{&arena};
    auto allocator = [&arena](usize size, usize alignment) {
        return arena.allocate(size, alignment);
    };
    auto const ranges = SAMPLE_PAGES / n;
    auto const pages = static_cast<double>(ranges * n);
    for (usize sample = 0; sample < SAMPLES; ++sample) {
        arena.reset();
        usize visits = 0;
        CountingPhysToVirt const counting{&visits};
        auto ok = true;

        auto const t0 = Clock::now();
        for (usize r = 0; r < ranges; ++r) {
            ok &= Engine::map(arena.root(), counting, allocator, deposits,
                              BASE_VA + r * n * PAGE_SIZE, BASE_PA + misalign + r * n * PAGE_SIZE,
                              n, USER_RW, FRESH)
                      .is_ok();
        }
        auto const t1 = Clock::now();
        auto const map_visits = visits;

        visits = 0;
        auto const t2 = Clock::now();
        for (usize r = 0; r < ranges; ++r) {
            ok &= Engine::protect(arena.root(), counting, deposits, BASE_VA + r * n * PAGE_SIZE, n,
                                  USER_RO)
                      .is_ok();
        }
        auto const t3 = Clock::now();
        auto const protect_visits = visits;

        visits = 0;
        auto const t4 = Clock::now();
        for (usize r = 0; r < ranges; ++r) {
            ok &= Engine::unmap(arena.root(), counting, deposits, BASE_VA + r * n * PAGE_SIZE, n)
                      .is_ok();
        }
        auto const t5 = Clock::now();
        auto const unmap_visits = visits;

        if (!ok) {
            std::fprintf(stderr, "the engine failed at %zu pages\n", n);
            std::exit(1);
        }
        recorder.add(name("map", leaves, n), "ns/page", "ns", elapsed_ns(t0, t1) / pages);
        recorder.add(name("map", leaves, n), "visits/page", "tables",
                     static_cast<double>(map_visits) / pages);
        recorder.add(name("protect", leaves, n), "ns/page", "ns", elapsed_ns(t2, t3) / pages);
        recorder.add(name("protect", leaves, n), "visits/page", "tables",
                     static_cast<double>(protect_visits) / pages);
        recorder.add(name("unmap", leaves, n), "ns/page", "ns", elapsed_ns(t4, t5) / pages);
        recorder.add(name("unmap", leaves, n), "visits/page", "tables",
                     static_cast<double>(unmap_visits) / pages);
    }
}

/// What PageTable::map_pages_bulk() does with a batch of |n| frames: one
/// engine call per page, since the frames are not contiguous (every other
/// page here). A sample is |SAMPLE_PAGES / n| batches, side by side.
auto run_bulk(Recorder &recorder, usize const n) -> void {
    Arena arena;
    CountedDeposits deposits{&arena};
    auto allocator = [&arena](usize size, usize alignment) {
        return arena.allocate(size, alignment);
    };
    auto const pages = SAMPLE_PAGES - SAMPLE_PAGES % n;
    for (usize sample = 0; sample < SAMPLES; ++sample) {
        arena.reset();
        usize visits = 0;
        CountingPhysToVirt const counting{&visits};
        auto ok = true;
        auto const t0 = Clock::now();
        for (usize i = 0; i < pages; ++i) {
            ok &= Engine::map(arena.root(), counting, allocator, deposits, BASE_VA + i * PAGE_SIZE,
                              BASE_PA + 2 * i * PAGE_SIZE, 1, USER_RW,
                              MapOptions::ErrorIfExisting | MapOptions::KeepOnFailure)
                      .is_ok();
        }
        auto const t1 = Clock::now();
        if (!ok) {
            std::fprintf(stderr, "the engine failed in a bulk map\n");
            std::exit(1);
        }
        recorder.add(name("bulk-map", "4K", n), "ns/page", "ns",
                     elapsed_ns(t0, t1) / static_cast<double>(pages));
        recorder.add(name("bulk-map", "4K", n), "visits/page", "tables",
                     static_cast<double>(visits) / static_cast<double>(pages));
    }
}

/// Two yardsticks for the walks' time, on the same machine:
///
///   - zeroing a 4 KiB page, which every page a fault allocates costs: 1024
///     distinct pages a sample, as freshly allocated ones would be;
///   - writing entries into a table with no walk at all, the part of a map
///     no walk can save: SAMPLE_PAGES of them a sample.
auto yardsticks(Recorder &recorder) -> void {
    constexpr usize ZEROED = 1024;
    std::vector<unsigned char> pages(ZEROED * PAGE_SIZE);
    std::vector<u64> entries(SAMPLE_PAGES);
    for (usize sample = 0; sample < SAMPLES; ++sample) {
        auto const t0 = Clock::now();
        for (usize i = 0; i < ZEROED; ++i) {
            std::memset(pages.data() + i * PAGE_SIZE, 0, PAGE_SIZE);
        }
        asm volatile("" : : "r"(pages.data()) : "memory");
        auto const t1 = Clock::now();
        for (usize i = 0; i < SAMPLE_PAGES; ++i) {
            entries[i] = (BASE_PA + i * PAGE_SIZE) | 0x7 | sample;
        }
        asm volatile("" : : "r"(entries.data()) : "memory");
        auto const t2 = Clock::now();
        recorder.add("yardstick zero-page", "ns/page", "ns",
                     elapsed_ns(t0, t1) / static_cast<double>(ZEROED));
        recorder.add("yardstick store-entry", "ns/page", "ns",
                     elapsed_ns(t1, t2) / static_cast<double>(SAMPLE_PAGES));
    }
}

auto print_row(Recorder const &recorder, char const *const op, char const *const leaves,
               usize const n, usize const floor) -> void {
    auto const what = name(op, leaves, n);
    auto const visits_per_page = recorder.median(what, "visits/page");
    auto const visits = visits_per_page * static_cast<double>(n);
    std::printf("%-8s | %-6s | %6zu | %8.0f | %6.2f | %5zu | %8.1fx | %8.2f\n", op, leaves, n,
                visits, visits_per_page, floor, visits / static_cast<double>(floor),
                recorder.median(what, "ns/page"));
}
} // namespace

/// No argument: a summary of the medians. `csv`: every raw sample, for a
/// performance report (docs/perf/TEMPLATE.md, scripts/perf_stats.py).
auto main(int argc, char **argv) -> int {
    auto const csv = argc == 2 && std::strcmp(argv[1], "csv") == 0;
    if (argc != 1 && !csv) {
        std::fprintf(stderr, "usage: page-walk-bench [csv]\n");
        return 2;
    }

    // 4 KiB leaves: frames one page off a 2 MiB boundary, so no step fits a
    // larger leaf. 2 MiB leaves: aligned frames, deposits always there.
    constexpr usize SMALL[] = {1, 512, 32768};
    constexpr usize LARGE[] = {512, 32768};
    constexpr usize BATCH = 32;
    auto const measure = [&](Recorder &recorder) {
        for (auto const n: SMALL) {
            run(recorder, "4K", n, PAGE_SIZE);
        }
        for (auto const n: LARGE) {
            run(recorder, "2M", n, 0);
        }
        run_bulk(recorder, BATCH);
        yardsticks(recorder);
    };

    // Once untimed first: a process that has just started may run on a
    // slower core, or at a lower clock, until the system sees it busy.
    Recorder warm_up;
    measure(warm_up);
    Recorder recorder;
    measure(recorder);

    if (csv) {
        std::printf("# page-walk-bench csv: %zu samples a case, %zu pages a sample\n", SAMPLES,
                    SAMPLE_PAGES);
        recorder.print_csv();
        return 0;
    }

    std::printf("medians of %zu samples, %zu pages a sample\n", SAMPLES, SAMPLE_PAGES);
    std::printf("%-8s | %-6s | %6s | %8s | %6s | %5s | %9s | %8s\n", "op", "leaves", "pages",
                "visits", "/page", "floor", "/floor", "ns/page");
    std::printf("---------+--------+--------+----------+--------+-------+-----------+---------\n");
    for (auto const n: SMALL) {
        auto const floor = floor_tables(BASE_VA, n, true);
        for (auto const *const op: {"map", "protect", "unmap"}) {
            print_row(recorder, op, "4K", n, floor);
        }
    }
    for (auto const n: LARGE) {
        auto const floor = floor_tables(BASE_VA, n, false);
        for (auto const *const op: {"map", "protect", "unmap"}) {
            print_row(recorder, op, "2M", n, floor);
        }
    }
    print_row(recorder, "bulk-map", "4K", BATCH, floor_tables(BASE_VA, BATCH, true));
    std::printf("\nyardsticks: zeroing a 4 KiB page %.1f ns; storing one entry, no walk, %.2f ns\n",
                recorder.median("yardstick zero-page", "ns/page"),
                recorder.median("yardstick store-entry", "ns/page"));
    return 0;
}
