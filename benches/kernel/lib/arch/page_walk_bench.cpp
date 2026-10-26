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
#include <chrono>
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

struct Measured {
    usize visits = 0;
    double ns = 0;
};

using Clock = std::chrono::steady_clock;

auto elapsed_ns(Clock::time_point const from, Clock::time_point const to) -> double {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
}

/// Maps, protects and unmaps |n| pages, |rounds| times, each round on a
/// fresh table, and gives the visits of one round and the mean time of each
/// operation.
struct Round {
    Measured map;
    Measured protect;
    Measured unmap;
};

auto run(usize const n, PhysAddr const misalign, usize const rounds) -> Round {
    Arena arena;
    CountedDeposits deposits{&arena};
    auto allocator = [&arena](usize size, usize alignment) {
        return arena.allocate(size, alignment);
    };
    Round total;
    for (usize round = 0; round < rounds; ++round) {
        arena.reset();
        usize visits = 0;
        CountingPhysToVirt const counting{&visits};

        auto const t0 = Clock::now();
        auto const mapped = Engine::map(arena.root(), counting, allocator, deposits, BASE_VA,
                                        BASE_PA + misalign, n, USER_RW, FRESH);
        auto const t1 = Clock::now();
        auto const map_visits = visits;

        visits = 0;
        auto const t2 = Clock::now();
        auto const protected_pages =
            Engine::protect(arena.root(), counting, deposits, BASE_VA, n, USER_RO);
        auto const t3 = Clock::now();
        auto const protect_visits = visits;

        visits = 0;
        auto const t4 = Clock::now();
        auto const unmapped = Engine::unmap(arena.root(), counting, deposits, BASE_VA, n);
        auto const t5 = Clock::now();
        auto const unmap_visits = visits;

        if (mapped.is_err() || protected_pages.is_err() || unmapped.is_err()) {
            std::printf("the engine failed at %zu pages\n", n);
            std::exit(1);
        }
        total.map = {map_visits, total.map.ns + elapsed_ns(t0, t1)};
        total.protect = {protect_visits, total.protect.ns + elapsed_ns(t2, t3)};
        total.unmap = {unmap_visits, total.unmap.ns + elapsed_ns(t4, t5)};
    }
    total.map.ns /= static_cast<double>(rounds);
    total.protect.ns /= static_cast<double>(rounds);
    total.unmap.ns /= static_cast<double>(rounds);
    return total;
}

/// What PageTable::map_pages_bulk() does with a batch: one engine call per
/// page, since the frames are not contiguous. |n| frames, every other page.
auto run_bulk(usize const n, usize const rounds) -> Measured {
    Arena arena;
    CountedDeposits deposits{&arena};
    auto allocator = [&arena](usize size, usize alignment) {
        return arena.allocate(size, alignment);
    };
    Measured total;
    for (usize round = 0; round < rounds; ++round) {
        arena.reset();
        usize visits = 0;
        CountingPhysToVirt const counting{&visits};
        auto const t0 = Clock::now();
        for (usize i = 0; i < n; ++i) {
            auto const mapped =
                Engine::map(arena.root(), counting, allocator, deposits, BASE_VA + i * PAGE_SIZE,
                            BASE_PA + 2 * i * PAGE_SIZE, 1, USER_RW,
                            MapOptions::ErrorIfExisting | MapOptions::KeepOnFailure);
            if (mapped.is_err()) {
                std::printf("the engine failed at page %zu of a bulk map\n", i);
                std::exit(1);
            }
        }
        auto const t1 = Clock::now();
        total = {visits, total.ns + elapsed_ns(t0, t1)};
    }
    total.ns /= static_cast<double>(rounds);
    return total;
}

auto print(char const *const op, char const *const leaves, usize const n, Measured const m,
           usize const floor) -> void {
    std::printf("%-8s | %-6s | %6zu | %7zu | %6.2f | %5zu | %8.1fx | %8.2f\n", op, leaves, n,
                m.visits, static_cast<double>(m.visits) / static_cast<double>(n), floor,
                static_cast<double>(m.visits) / static_cast<double>(floor),
                m.ns / static_cast<double>(n));
}

/// Two yardsticks for the walks' time, on the same machine:
///
///   - zeroing one 4 KiB page, which every page a fault allocates costs;
///   - writing |n| entries into tables with no walk at all, the part of a
///     map no walk can save.
struct Yardsticks {
    double zero_page_ns;
    double store_entry_ns;
};

auto yardsticks(usize const n, usize const rounds) -> Yardsticks {
    std::vector<unsigned char> page(PAGE_SIZE);
    std::vector<u64> entries(n);
    auto const t0 = Clock::now();
    for (usize round = 0; round < rounds; ++round) {
        std::memset(page.data(), static_cast<int>(round), PAGE_SIZE);
        asm volatile("" : : "r"(page.data()) : "memory");
    }
    auto const t1 = Clock::now();
    for (usize round = 0; round < rounds; ++round) {
        for (usize i = 0; i < n; ++i) {
            entries[i] = (BASE_PA + i * PAGE_SIZE) | 0x7 | round;
        }
        asm volatile("" : : "r"(entries.data()) : "memory");
    }
    auto const t2 = Clock::now();
    return {elapsed_ns(t0, t1) / static_cast<double>(rounds),
            elapsed_ns(t1, t2) / static_cast<double>(rounds * n)};
}

/// Enough rounds that a case takes a few milliseconds in all.
auto rounds_for(usize const n) -> usize {
    auto const rounds = (usize{1} << 21) / n;
    if (rounds < 16) {
        return 16;
    }
    return rounds;
}
} // namespace

auto main() -> int {
    std::printf("%-8s | %-6s | %6s | %7s | %6s | %5s | %9s | %8s\n", "op", "leaves", "pages",
                "visits", "/page", "floor", "/floor", "ns/page");
    std::printf("---------+--------+--------+---------+--------+-------+-----------+---------\n");

    // 4 KiB leaves: frames one page off a 2 MiB boundary, so no step fits a
    // larger leaf.
    for (usize const n: {usize{1}, usize{512}, usize{32768}}) {
        auto const round = run(n, PAGE_SIZE, rounds_for(n));
        auto const floor = floor_tables(BASE_VA, n, true);
        print("map", "4K", n, round.map, floor);
        print("protect", "4K", n, round.protect, floor);
        print("unmap", "4K", n, round.unmap, floor);
    }

    // 2 MiB leaves: aligned frames, deposits always there.
    for (usize const n: {usize{512}, usize{32768}}) {
        auto const round = run(n, 0, rounds_for(n));
        auto const floor = floor_tables(BASE_VA, n, false);
        print("map", "2M", n, round.map, floor);
        print("protect", "2M", n, round.protect, floor);
        print("unmap", "2M", n, round.unmap, floor);
    }

    // A coalescer's batch through map_pages_bulk(): one call per page.
    constexpr usize BATCH = 32;
    print("bulk-map", "4K", BATCH, run_bulk(BATCH, rounds_for(BATCH)),
          floor_tables(BASE_VA, BATCH, true));

    auto const yard = yardsticks(32768, 64);
    std::printf("\nyardsticks: zeroing one 4 KiB page %.1f ns; storing one entry, no walk, "
                "%.2f ns\n",
                yard.zero_page_ns, yard.store_entry_ns);
    return 0;
}
