/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host benchmark for x86's page-table walks (page_tables::X86PageTableImpl,
/// four levels), the measurements of docs/perf/261025/page-walk.md. Until
/// stage 3 of that report it drove the generic paging engine
/// (arch::paging::Paging) the same way.
///
/// Per operation it prints:
///
///   visits     how many times the walk entered a table: every call of the
///              table() hook is one, and the one here counts them. Exact,
///              and the same on any machine.
///   floor      how many distinct tables the range touches: what a walk that
///              went down to each table once, and then stepped through it,
///              would visit.
///   ns/page    host time per page. The walks are their own software work on
///              ordinary memory, with no TLB in them, so the host's number is
///              a fair one for that work; it is not what the kernel adds
///              around it (the lock, interrupts off, the TLB shootdown).
///
/// The tables live in one arena whose addresses stand in for physical ones
/// (an identity table()). Leaf frames are never touched, so any address will
/// do for them. The table splits its large leaves, as a user address space's
/// does, but its deposits are counted, not kept: a reservation always
/// succeeds, and a withdrawal hands out a fresh table.
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <page_tables/mmu_format.hpp>
#include <page_tables/page_tables.hpp>

namespace {
using arch::paging::MapOptions;
using arch::paging::MmuFlags;
using ours::PhysAddr;
using ours::VirtAddr;
using page_tables::PageTableLevel;

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

/// The tables of the arena as an x86 page table that splits its large
/// leaves, counting every table a walk enters (see the file comment).
class BenchTable final: public page_tables::X86PageTableImpl<BenchTable> {
    friend class page_tables::X86PageTableImpl<BenchTable>;

  public:
    BenchTable(Arena *arena, usize *visits) : m_arena(arena), m_visits(visits) {}

    using X86PageTableImpl::map;
    using X86PageTableImpl::map_frames;
    using X86PageTableImpl::protect;
    using X86PageTableImpl::unmap;

  private:
    using Format = page_tables::MmuFormat;

    static constexpr auto top_level() -> PageTableLevel {
        return PageTableLevel::PageMapLevel4;
    }

    [[nodiscard]]
    auto root() const -> PhysAddr {
        return m_arena->root();
    }

    [[nodiscard]]
    auto table(PhysAddr const pa) const -> arch::PteVal * {
        *m_visits += 1;
        return reinterpret_cast<arch::PteVal *>(pa);
    }

    static constexpr auto splits_large_leaves() -> bool {
        return true;
    }

    auto withdraw_table(PhysAddr) -> PhysAddr {
        return m_arena->allocate(PAGE_SIZE, PAGE_SIZE);
    }

    auto discard_table(PhysAddr) -> void {}

    auto reserve_table(PhysAddr) -> bool {
        return true;
    }

    auto discard_all_tables(PhysAddr) -> void {}

    auto allocate_table() -> PhysAddr {
        return m_arena->allocate(PAGE_SIZE, PAGE_SIZE);
    }

    auto free_table(PhysAddr) -> void {}

    Arena *m_arena;
    usize *m_visits;
};

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

/// The walks' calls, each in a function of its own that is never inlined,
/// so that a profile tells them apart: inlined, map and protect showed in a
/// flame graph as run() itself. The walks are inlined into each as they are
/// into PageTable, so the profile keeps their shape; the wrapper adds one
/// call per walk.
[[gnu::noinline]]
auto table_map(BenchTable &table, VirtAddr const va, PhysAddr const pa, usize const n,
               MapOptions const options) -> bool {
    return table.map(va, pa, n, USER_RW, options, nullptr) == ours::Status::Ok;
}

[[gnu::noinline]]
auto table_map_frames(BenchTable &table, VirtAddr const va, PhysAddr const *const frames,
                      usize const n) -> bool {
    return table.map_frames(va, frames, n, USER_RW,
                            MapOptions::ErrorIfExisting | MapOptions::KeepOnFailure,
                            nullptr) == ours::Status::Ok;
}

[[gnu::noinline]]
auto table_protect(BenchTable &table, VirtAddr const va, usize const n) -> bool {
    return table.protect(va, n, USER_RO) == ours::Status::Ok;
}

[[gnu::noinline]]
auto table_unmap(BenchTable &table, VirtAddr const va, usize const n) -> bool {
    return table.unmap(va, n) == ours::Status::Ok;
}

/// |SAMPLES| samples of map, protect and unmap of ranges of |n| pages, with
/// the frames |misalign| off a 2 MiB boundary. Each sample starts from an
/// empty table. Gives time and table visits per page.
auto run(Recorder &recorder, char const *const leaves, usize const n, PhysAddr const misalign)
    -> void {
    Arena arena;
    usize visits = 0;
    BenchTable table(&arena, &visits);
    auto const ranges = SAMPLE_PAGES / n;
    auto const pages = static_cast<double>(ranges * n);
    for (usize sample = 0; sample < SAMPLES; ++sample) {
        arena.reset();
        visits = 0;
        auto ok = true;

        auto const t0 = Clock::now();
        for (usize r = 0; r < ranges; ++r) {
            ok &= table_map(table, BASE_VA + r * n * PAGE_SIZE,
                            BASE_PA + misalign + r * n * PAGE_SIZE, n, FRESH);
        }
        auto const t1 = Clock::now();
        auto const map_visits = visits;

        visits = 0;
        auto const t2 = Clock::now();
        for (usize r = 0; r < ranges; ++r) {
            ok &= table_protect(table, BASE_VA + r * n * PAGE_SIZE, n);
        }
        auto const t3 = Clock::now();
        auto const protect_visits = visits;

        visits = 0;
        auto const t4 = Clock::now();
        for (usize r = 0; r < ranges; ++r) {
            ok &= table_unmap(table, BASE_VA + r * n * PAGE_SIZE, n);
        }
        auto const t5 = Clock::now();
        auto const unmap_visits = visits;

        if (!ok) {
            std::fprintf(stderr, "a walk failed at %zu pages\n", n);
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

/// What PageTable::map_pages_bulk() does with a batch of |n| frames that are
/// not contiguous (every other page here): one call for the batch
/// (map_frames()). A sample is |SAMPLE_PAGES / n| batches, side by side.
auto run_bulk(Recorder &recorder, usize const n) -> void {
    Arena arena;
    usize visits = 0;
    BenchTable table(&arena, &visits);
    auto const batches = SAMPLE_PAGES / n;
    auto const pages = batches * n;
    std::vector<PhysAddr> frames(pages);
    for (usize i = 0; i < pages; ++i) {
        frames[i] = BASE_PA + 2 * i * PAGE_SIZE;
    }
    for (usize sample = 0; sample < SAMPLES; ++sample) {
        arena.reset();
        visits = 0;
        auto ok = true;
        auto const t0 = Clock::now();
        for (usize b = 0; b < batches; ++b) {
            ok &= table_map_frames(table, BASE_VA + b * n * PAGE_SIZE, frames.data() + b * n, n);
        }
        auto const t1 = Clock::now();
        if (!ok) {
            std::fprintf(stderr, "a walk failed in a bulk map\n");
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
/// `spin SECONDS CASE`: one case's work over and over, printing nothing but
/// `ready`, for a sampling profiler to attach to (scripts/flamegraph.py).
/// CASE is 4k-1, 4k-512, 4k-32768, 2m-512, 2m-32768, bulk-32 or all.
auto main(int argc, char **argv) -> int {
    auto const csv = argc == 2 && std::strcmp(argv[1], "csv") == 0;
    auto const spin = argc == 4 && std::strcmp(argv[1], "spin") == 0;
    if (argc != 1 && !csv && !spin) {
        std::fprintf(stderr, "usage: page-walk-bench [csv | spin SECONDS CASE]\n");
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

    if (spin) {
        std::string const which = argv[3];
        auto const one = [&](Recorder &recorder) {
            if (which == "4k-1") {
                run(recorder, "4K", 1, PAGE_SIZE);
            } else if (which == "4k-512") {
                run(recorder, "4K", 512, PAGE_SIZE);
            } else if (which == "4k-32768") {
                run(recorder, "4K", 32768, PAGE_SIZE);
            } else if (which == "2m-512") {
                run(recorder, "2M", 512, 0);
            } else if (which == "2m-32768") {
                run(recorder, "2M", 32768, 0);
            } else if (which == "bulk-32") {
                run_bulk(recorder, BATCH);
            } else {
                measure(recorder);
            }
        };
        // The profiler attaches on this line: before main() a process may
        // still be stopped in the loader, and its samples would be that.
        std::printf("ready\n");
        std::fflush(stdout);
        auto const until = Clock::now() + std::chrono::seconds(std::atoi(argv[2]));
        while (Clock::now() < until) {
            Recorder discarded;
            one(discarded);
        }
        return 0;
    }

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
