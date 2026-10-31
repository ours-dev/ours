/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PAGE_TABLES_PAGE_TABLES_HPP
#define PAGE_TABLES_PAGE_TABLES_HPP 1

#include <atomic>

#include <arch/paging/controls.hpp>
#include <arch/paging/mmu_flags.hpp>
#include <arch/paging/x86_pagings.hpp>
#include <arch/types.hpp>
#include <arch/x86/macro/mmu.hpp>
#include <ours/status.hpp>
#include <ustl/option.hpp>

namespace page_tables {
using arch::PhysAddr;
using arch::PteVal;
using arch::VirtAddr;
using arch::paging::MapOptions;
using arch::paging::MmuFlags;
using PageTableLevel = arch::paging::X86PagingLevel;

/// The shift of the address bits that index a table at |level|, which is
/// also the log2 of the size an entry at that level covers.
constexpr auto shift(PageTableLevel const level) -> usize {
    return X86_PT_SHIFT + usize(level) * 9;
}

/// The bytes one entry at |level| covers: 4 KiB at the last level, 2 MiB,
/// 1 GiB, 512 GiB above it.
constexpr auto page_size(PageTableLevel const level) -> usize {
    return usize{1} << shift(level);
}

/// The entry of a table at |level| that |va| falls under.
constexpr auto index(PageTableLevel const level, VirtAddr const va) -> usize {
    return (va >> shift(level)) & (X86_MAX_PTES - 1);
}

/// The level of the tables an entry at |level| points to.
constexpr auto lower(PageTableLevel const level) -> PageTableLevel {
    return PageTableLevel(usize(level) - 1);
}

constexpr auto is_present(PteVal const entry) -> bool {
    return (entry & X86_MMUF_PRESENT) != 0;
}

/// Whether a present |entry| at |level| maps memory rather than pointing to
/// a table. Every entry at the last level does; above it, only one with the
/// page-size bit, which the hardware reads at the two levels that may hold a
/// large leaf (2 MiB and 1 GiB) and nowhere else.
constexpr auto is_leaf(PageTableLevel const level, PteVal const entry) -> bool {
    if (level == PageTableLevel::PageTable) {
        return true;
    }
    if (level == PageTableLevel::PageDirectory ||
        level == PageTableLevel::PageDirectoryPointerTable) {
        return (entry & X86_MMUF_PAGE_SIZE) != 0;
    }
    return false;
}

/// The physical address an entry holds: the table it points to, or the
/// memory a leaf maps.
constexpr auto address(PteVal const entry) -> PhysAddr {
    return entry & X86_PG_FRAME;
}

/// One entry of a table, as the walks read and write it. A type of its own,
/// not a bare PteVal: an entry and a cursor's fields would otherwise both be
/// unsigned long, and the compiler, unable to tell a store to one from a
/// store to the other, would load every cursor field from memory again
/// after every entry a loop writes, and store it back. A store to a Pte
/// cannot change an object that is not one (the strict aliasing rule), so a
/// walk's cursor stays in registers.
struct Pte {
    PteVal value;
};

/// What a virtual address translates to: the physical address it reaches,
/// the size of the leaf that maps it, and that leaf's permissions.
struct Mapping {
    PhysAddr phys;
    usize size;
    MmuFlags flags;
};

/// The highest physical address x86 can name: 52 bits.
constexpr PhysAddr MAX_PHYS_ADDR = (PhysAddr{1} << 52) - 1;

/// Whether a leaf may be written at |level|, an upper one: 2 MiB and 1 GiB
/// leaves exist, 512 GiB ones do not.
constexpr auto can_hold_leaf(PageTableLevel const level) -> bool {
    return level == PageTableLevel::PageDirectory ||
           level == PageTableLevel::PageDirectoryPointerTable;
}

/// What free_tables() did: the tables it gave back, and the leaves it
/// stepped over.
struct TableTeardown {
    usize tables = 0;
    usize leaves = 0;
};

/// Where a walk over a range is: the next address, and the bytes still to do.
struct RangeCursor {
    VirtAddr va;
    usize remaining;
};

/// Where a map is: the next address and the bytes still to map, and the
/// frames they go to. The frames are runs of |run_size| bytes from the
/// addresses at |runs|: one run as long as the whole range for a contiguous
/// map, a page a run for map_frames(). One kind of cursor serves both, so
/// that the walk of a map is emitted once; what it costs is a check a page
/// for the end of the run, which a contiguous map never reaches.
struct MapCursor {
    VirtAddr va;
    usize remaining;
    PhysAddr const *runs;
    usize run_size;
    usize run_used = 0;

    /// The frame the next byte goes to.
    [[nodiscard]]
    constexpr auto pa() const -> PhysAddr {
        return *runs + run_used;
    }

    /// The bytes left in the current run: the most one leaf here may cover.
    [[nodiscard]]
    constexpr auto run_left() const -> usize {
        return run_size - run_used;
    }

    /// Moves on by |bytes|, which do not go past the current run.
    constexpr auto consume(usize const bytes) -> void {
        va += bytes;
        remaining -= bytes;
        run_used += bytes;
        if (run_used == run_size) {
            ++runs;
            run_used = 0;
        }
    }

    /// Moves on by |bytes|, across as many runs as they take: what Skip
    /// steps over may be a whole 2 MiB leaf of a page a run.
    constexpr auto skip(usize bytes) -> void {
        while (bytes != 0) {
            auto chunk = run_left();
            if (chunk > bytes) {
                chunk = bytes;
            }
            consume(chunk);
            bytes -= chunk;
        }
    }
};

/// The walks of an x86 page table, written once for every kind of table that
/// derives from it (`class Mmu: public X86PageTableImpl<Mmu>`). The walks
/// hold no state and take no lock: the derived class owns the root and the
/// lock, and calls them under it, so they are protected.
///
/// |Derived| supplies what differs between kinds of table, and the walks ask
/// it through `derived()`, resolved at compile time:
///
///   - `Format`: a type saying what an entry's bits mean (MmuFormat);
///   - `top_level() -> PageTableLevel`: the level of its root table;
///   - `root() -> PhysAddr`: its root table;
///   - `table(PhysAddr) -> Pte *`: where the table at a physical address
///     is seen from the address space the walk runs in;
///   - `splits_large_leaves() -> bool`: whether its 2 MiB leaves each have a
///     table set aside, so that a change to part of one splits it;
///   - `withdraw_table(PhysAddr owner) -> PhysAddr`: takes back a table set
///     aside with |owner|, the table holding the leaf, to split the leaf;
///   - `discard_table(PhysAddr owner)`: frees one of |owner|'s, when a whole
///     2 MiB leaf goes;
///   - `reserve_table(PhysAddr owner) -> bool`: sets a zeroed table aside
///     with |owner| for a 2 MiB leaf about to be written there, or says none
///     can be had;
///   - `allocate_table() -> PhysAddr`: a zeroed table, or 0 when none can be
///     had;
///   - `free_table(PhysAddr)`: gives a table back, allocate_table()'s
///     mirror;
///   - `discard_all_tables(PhysAddr owner)`: frees every table set aside
///     with |owner|, which is about to be freed itself.
///
/// Whether large leaves split is a question asked at run time where a split
/// may happen, not a type, so that no walk is emitted once for each kind of
/// table. The level is a run-time argument in the walks that visit one entry
/// a level (query, second_level_empty, free_tables), and a template parameter
/// in the two that walk a range (walk_range, map_range): a call that enters
/// four levels and does little in each costs about twice the instructions
/// when each level is a call of its own, and the range walks are the calls
/// made a page at a time (stage 6 of docs/perf/261025/page-walk.md).
template <typename Derived>
class X86PageTableImpl {
  protected:
    /// What |va| translates to, or none when nothing maps it.
    [[nodiscard]]
    auto query(VirtAddr const va) const -> ustl::Option<Mapping> {
        // Named in each body, where the derived class is complete: a member
        // alias would be read when the base is, before the derived class is.
        using Format = typename Derived::Format;
        auto level = derived().top_level();
        auto table = derived().root();
        for (;;) {
            auto const entry = derived().table(table)[index(level, va)].value;
            if (!is_present(entry)) {
                return ustl::NONE;
            }
            if (is_leaf(level, entry)) {
                auto const size = page_size(level);
                return ustl::Option<Mapping>(Mapping{
                    .phys = address(entry) + (va & (size - 1)),
                    .size = size,
                    .flags = Format::to_mmu_flags(entry, level),
                });
            }
            table = address(entry);
            level = lower(level);
        }
    }

    /// Unmaps [va, va + n pages) in one walk (walk_range()). A 2 MiB leaf
    /// the range covers whole gives back the table set aside for it; one it
    /// covers in part is split first, when the table splits its large leaves,
    /// and is InvArgs otherwise. On an error, what came before it is done.
    ///
    /// No TLB is invalidated: that is the caller's, for the whole range.
    [[nodiscard]]
    auto unmap(VirtAddr const va, usize const n) -> ours::Status {
        if (n == 0) {
            return ours::Status::Ok;
        }
        if (!range_is_valid(va, n)) {
            return ours::Status::InvArgs;
        }
        RangeCursor cursor{va, n * PAGE_SIZE};
        auto clear = [this](Pte &entry, PageTableLevel const level, PhysAddr const owner) {
            entry.value = 0;
            if (level == PageTableLevel::PageDirectory && derived().splits_large_leaves()) {
                derived().discard_table(owner);
            }
        };
        return walk_range<Derived::top_level()>(derived().root(), cursor, clear);
    }

    /// Takes permissions away and never gives one: the permission bits of
    /// every leaf in the range become what they were AND |flags|, and no
    /// other bit is touched. A permission is added only by mapping the page
    /// again, which names the frame. Splits as unmap() does; on an error,
    /// what came before it is done.
    ///
    /// No TLB is invalidated: that is the caller's, for the whole range.
    [[nodiscard]]
    auto protect(VirtAddr const va, usize const n, MmuFlags const flags) -> ours::Status {
        if (n == 0) {
            return ours::Status::Ok;
        }
        if (!range_is_valid(va, n)) {
            return ours::Status::InvArgs;
        }
        using Format = typename Derived::Format;
        RangeCursor cursor{va, n * PAGE_SIZE};
        // Worked out here, once: the walk is not inlined (it calls itself a
        // level at a time), so |flags| is not a constant in it, and per entry
        // the same conversion would cost twice the instructions.
        //
        // The masks are captured as two plain values, not as the Narrowing
        // that holds them: captured as the struct, the walk loaded them again
        // for every entry (15 instructions a page against 9), for a reason a
        // small example did not reproduce (stage 5 of
        // docs/perf/261025/page-walk.md has the counts).
        using Narrowing = typename Format::Narrowing;
        auto const narrowing = Format::narrowing(flags);
        auto const keep = narrowing.keep;
        auto const set = narrowing.set;
        auto narrow = [keep, set](Pte &entry, PageTableLevel, PhysAddr) {
            entry.value = Format::narrow(entry.value, Narrowing{keep, set});
        };
        return walk_range<Derived::top_level()>(derived().root(), cursor, narrow);
    }

    /// For every leaf in [va, va + n pages) whose accessed bit is set, asks
    /// |visit|(phys, size), with the memory the leaf maps and the leaf's size,
    /// and clears the bit when the answer is true; how many it cleared goes to
    /// |cleared|. Splits as unmap() does; on an error, what came before it is
    /// done.
    ///
    /// The bit is cleared by an atomic read-modify-write, not a load and a
    /// store: the hardware writes the accessed and dirty bits of an entry
    /// without the table's lock, and a store of what was loaded would undo a
    /// dirty bit set in between, once leaves are made without it
    /// (X86Pte::make() sets it in every writable leaf today).
    ///
    /// No TLB is invalidated: that is the caller's. Until it is, a CPU that
    /// holds a cleared leaf's translation goes on using it, and never sets
    /// the bit again: an access the next harvest does not see.
    template <typename Visitor>
    [[nodiscard]]
    auto harvest_accessed(VirtAddr const va, usize const n, Visitor &visit, usize *const cleared)
        -> ours::Status {
        report(cleared, 0);
        if (n == 0) {
            return ours::Status::Ok;
        }
        if (!range_is_valid(va, n)) {
            return ours::Status::InvArgs;
        }
        using Format = typename Derived::Format;
        RangeCursor cursor{va, n * PAGE_SIZE};
        usize count = 0;
        auto harvest = [&visit, &count](Pte &entry, PageTableLevel const level, PhysAddr) {
            if ((entry.value & Format::ACCESSED) == 0 ||
                !visit(address(entry.value), page_size(level))) {
                return;
            }
            std::atomic_ref<PteVal>(entry.value)
                .fetch_and(~Format::ACCESSED, std::memory_order_relaxed);
            count += 1;
        };
        auto const status = walk_range<Derived::top_level()>(derived().root(), cursor, harvest);
        report(cleared, count);
        return status;
    }

    /// Maps [va, va + n pages) to the frames from |pa| on, with a leaf as
    /// large as the range and its alignment allow at each step: 4 KiB, 2 MiB
    /// and 1 GiB in a table that does not split its large leaves; in one that
    /// does, 2 MiB only, each with a table set aside for it.
    ///
    /// |options| says what a page already mapped gets (Error, Skip or
    /// Overwrite) and what a failure part way leaves (Undo or Keep). With
    /// Undo, the pages the call got through are unmapped again, and |mapped|
    /// says 0; with Keep they stay, and |mapped| says how many.
    ///
    /// No TLB is invalidated: after an Overwrite or an undo, that is the
    /// caller's.
    [[nodiscard]]
    auto map(VirtAddr const va, PhysAddr const pa, usize const n, MmuFlags const flags,
             MapOptions const options, usize *const mapped) -> ours::Status {
        report(mapped, 0);
        if (!map_options_valid(options)) {
            return ours::Status::InvArgs;
        }
        if (n == 0) {
            return ours::Status::Ok;
        }
        if (!range_is_valid(va, n) || pa % PAGE_SIZE != 0) {
            return ours::Status::InvArgs;
        }
        auto const size = n * PAGE_SIZE;
        if (pa > MAX_PHYS_ADDR || size - 1 > MAX_PHYS_ADDR - pa) {
            return ours::Status::InvArgs;
        }
        MapCursor cursor{.va = va, .remaining = size, .runs = &pa, .run_size = size};
        return map_with(va, n, cursor, flags, options, mapped);
    }

    /// Maps |n| pages at |va| to the frames |frames| lists, one a page, in
    /// one walk. The frames need not be contiguous, so every leaf is a 4 KiB
    /// one. |options| and |mapped| as for map().
    [[nodiscard]]
    auto map_frames(VirtAddr const va, PhysAddr const *const frames, usize const n,
                    MmuFlags const flags, MapOptions const options, usize *const mapped)
        -> ours::Status {
        report(mapped, 0);
        if (!map_options_valid(options)) {
            return ours::Status::InvArgs;
        }
        if (n == 0) {
            return ours::Status::Ok;
        }
        if (!range_is_valid(va, n)) {
            return ours::Status::InvArgs;
        }
        for (usize i = 0; i < n; ++i) {
            if (frames[i] % PAGE_SIZE != 0 || frames[i] > MAX_PHYS_ADDR - (PAGE_SIZE - 1)) {
                return ours::Status::InvArgs;
            }
        }
        MapCursor cursor{
            .va = va, .remaining = n * PAGE_SIZE, .runs = frames, .run_size = PAGE_SIZE};
        return map_with(va, n, cursor, flags, options, mapped);
    }

    /// Whether the 2 MiB entry |va| falls under is empty, so that a 2 MiB
    /// leaf could be written there: no level above has a leaf over it, and
    /// the entry itself neither is a leaf nor points to a table. A table
    /// there, even one with nothing mapped in it, is not empty: a split
    /// leaves one behind and nothing frees it.
    [[nodiscard]]
    auto second_level_empty(VirtAddr const va) const -> bool {
        auto level = derived().top_level();
        auto table = derived().root();
        for (;;) {
            auto const entry = derived().table(table)[index(level, va)].value;
            if (!is_present(entry)) {
                return true;
            }
            if (level == PageTableLevel::PageDirectory || is_leaf(level, entry)) {
                return false;
            }
            table = address(entry);
            level = lower(level);
        }
    }

    /// Frees every table under the root's entries [first, last], deepest
    /// first, clearing each entry that pointed to one; in a table that splits
    /// its large leaves, also every table set aside with a 2 MiB-level table
    /// it frees: those are the tables kept for leaves still mapped, and
    /// nothing else will free them. The root itself is the caller's.
    ///
    /// Leaves are stepped over and counted, never freed: the frames they
    /// point to belong to whoever mapped them, and a large leaf holds data,
    /// not a table. A count other than zero means the table was torn down
    /// with something still mapped.
    ///
    /// Only for a table no CPU has loaded: nothing here flushes a TLB or a
    /// paging-structure cache. Unmapping never frees a table, so an address
    /// space keeps its tables for its whole life and they all come back here.
    auto free_tables(usize const first, usize const last) -> TableTeardown {
        TableTeardown result;
        free_below(derived().root(), derived().top_level(), first, last, result);
        return result;
    }

  private:
    static auto report(usize *const mapped, usize const pages) -> void {
        if (mapped != nullptr) {
            *mapped = pages;
        }
    }
    [[nodiscard]]
    auto derived() const -> Derived const & {
        return static_cast<Derived const &>(*this);
    }

    [[nodiscard]]
    auto derived() -> Derived & {
        return static_cast<Derived &>(*this);
    }

    /// Whether [va, va + n pages) is page aligned and does not wrap.
    [[nodiscard]]
    static constexpr auto range_is_valid(VirtAddr const va, usize const n) -> bool {
        if (va % PAGE_SIZE != 0 || n > (~usize{0}) / PAGE_SIZE) {
            return false;
        }
        return n * PAGE_SIZE - 1 <= (~VirtAddr{0}) - va;
    }

    /// Walks the range at |cursor| below the table at |table|, which is at
    /// level |Level|, once, table by table: each table on the way is entered
    /// once, and its entries are stepped through in a loop up to its end or
    /// the range's. For each entry:
    ///
    ///   - absent: the part of the range under it is passed over whole;
    ///   - a leaf the range covers whole: |action| is done to it;
    ///   - a 2 MiB leaf the range covers in part, in a table that splits its
    ///     large leaves: split into the table set aside for it, and gone into;
    ///   - any other leaf covered in part: InvArgs, since nothing can be
    ///     split without allocating;
    ///   - a table: gone into.
    ///
    /// A leaf can be covered in part only at the two ends of the range, so a
    /// split happens at most twice a walk, where the walk is anyway. |cursor|
    /// moves on as the range is done, and on an error stays where the walk
    /// stopped.
    ///
    /// The level is a template parameter: each level is a function of its
    /// own, and going down is a call the compiler can see through. With the
    /// level an argument, a one-page protect ran 249 instructions, and 117
    /// this way, for a copy of the loop a level and an action.
    template <PageTableLevel Level, typename Action>
    [[nodiscard]]
    auto walk_range(PhysAddr const table, RangeCursor &cursor, Action &action) -> ours::Status {
        if constexpr (Level == PageTableLevel::PageTable) {
            walk_last_level(table, cursor, action);
            return ours::Status::Ok;
        } else {
            auto *const entries = derived().table(table);
            auto const size = page_size(Level);
            for (auto at = index(Level, cursor.va); at < X86_MAX_PTES && cursor.remaining != 0;
                 ++at) {
                auto &entry = entries[at];
                // The part of the range under this entry.
                auto const offset = cursor.va & (size - 1);
                auto step = size - offset;
                if (step > cursor.remaining) {
                    step = cursor.remaining;
                }

                if (!is_present(entry.value)) {
                    cursor.va += step;
                    cursor.remaining -= step;
                    continue;
                }
                if (is_leaf(Level, entry.value)) {
                    if (offset == 0 && step == size) {
                        action(entry, Level, table);
                        cursor.va += step;
                        cursor.remaining -= step;
                        continue;
                    }
                    if (Level != PageTableLevel::PageDirectory ||
                        !derived().splits_large_leaves()) {
                        return ours::Status::InvArgs;
                    }
                    split(entry, table);
                }
                auto const status = walk_range<lower(Level)>(address(entry.value), cursor, action);
                if (status != ours::Status::Ok) {
                    return status;
                }
            }
            return ours::Status::Ok;
        }
    }

    /// walk_range() at the last level, as a tight loop: every present entry
    /// there is a leaf of one page, covered whole, so none of the upper
    /// levels' questions arise.
    template <typename Action>
    auto walk_last_level(PhysAddr const table, RangeCursor &cursor, Action &action) -> void {
        auto *const entries = derived().table(table);
        for (auto at = index(PageTableLevel::PageTable, cursor.va);
             at < X86_MAX_PTES && cursor.remaining != 0; ++at) {
            if (is_present(entries[at].value)) {
                action(entries[at], PageTableLevel::PageTable, table);
            }
            cursor.va += PAGE_SIZE;
            cursor.remaining -= PAGE_SIZE;
        }
    }

    /// The walk of a map, and what its failure leaves: |cursor| is at |va|,
    /// with |n| pages to go.
    [[nodiscard]]
    auto map_with(VirtAddr const va, usize const n, MapCursor &cursor, MmuFlags const flags,
                  MapOptions const options, usize *const mapped) -> ours::Status {
        auto const status =
            map_range<Derived::top_level()>(derived().root(), cursor, flags, options);
        if (status == ours::Status::Ok) {
            report(mapped, n);
            return status;
        }
        // How far the call got: every entry before the failed one is done,
        // written or skipped; the failed one wrote nothing.
        auto const got = (cursor.va - va) / PAGE_SIZE;
        if (!!(options & MapOptions::KeepOnFailure)) {
            report(mapped, got);
        } else if (got != 0) {
            // Whole leaves this call wrote, and with Skip what it stepped
            // over, which may be part of a leaf: a part that cannot be split
            // (no deposits) stays, and is all an undo can fail to take.
            auto _ = unmap(va, got);
        }
        return status;
    }

    /// Maps what is left of |cursor| below the table at |table|, which is at
    /// level |Level|, table by table: each table is entered once and its
    /// entries are filled in a loop up to its end or the range's.
    ///
    ///   - An absent entry gets a leaf when the rest of the run fits one
    ///     (aligned, long enough, and allowed: may_be_leaf()), and a table
    ///     from allocate_table() otherwise, which the walk goes down into.
    ///   - A present leaf is what |options| says: skipped, an error, or
    ///     written over. Written over in place when it is the size the step
    ///     maps, keeping a table set aside for it, which the new leaf needs
    ///     as much; split and gone into when it is larger, in a table that
    ///     splits its large leaves; InvArgs otherwise.
    ///   - A present table is gone into, and its entries are filled: a leaf
    ///     never takes a table's place.
    ///
    /// On x86 a present entry may be changed directly; the TLB is the
    /// caller's (map()).
    ///
    /// The level is a template parameter, as in walk_range(), for the same
    /// reason: a one-page map ran 279 instructions with it an argument, and
    /// 151 this way.
    template <PageTableLevel Level>
    [[nodiscard]]
    auto map_range(PhysAddr const table, MapCursor &cursor, MmuFlags const flags,
                   MapOptions const options) -> ours::Status {
        using Format = typename Derived::Format;
        if constexpr (Level == PageTableLevel::PageTable) {
            return map_last_level(table, cursor,
                                  Format::leaf_bits(PageTableLevel::PageTable, flags), options);
        } else {
            auto *const entries = derived().table(table);
            auto const size = page_size(Level);
            for (auto at = index(Level, cursor.va); at < X86_MAX_PTES && cursor.remaining != 0;
                 ++at) {
                auto &entry = entries[at];
                // Whether a leaf of this level's size would go here: the rest
                // of the run is that long, and both addresses are aligned to
                // it.
                auto const fits = size <= cursor.run_left() && (cursor.pa() & (size - 1)) == 0 &&
                                  (cursor.va & (size - 1)) == 0;

                if (is_present(entry.value) && is_leaf(Level, entry.value)) {
                    if (!!(options & MapOptions::SkipIfExisting)) {
                        // The rest of the leaf that is there, as far as the
                        // range goes, stays as it is.
                        auto step = size - (cursor.va & (size - 1));
                        if (step > cursor.remaining) {
                            step = cursor.remaining;
                        }
                        cursor.skip(step);
                        continue;
                    }
                    if (!(options & MapOptions::OverwriteIfExisting)) {
                        return ours::Status::AlreadyExists;
                    }
                    if (fits) {
                        entry.value = Format::leaf(cursor.pa(), Format::leaf_bits(Level, flags));
                        cursor.consume(size);
                        continue;
                    }
                    if (Level != PageTableLevel::PageDirectory ||
                        !derived().splits_large_leaves()) {
                        return ours::Status::InvArgs;
                    }
                    split(entry, table);
                } else if (!is_present(entry.value)) {
                    if (may_be_leaf(Level, table, fits)) {
                        entry.value = Format::leaf(cursor.pa(), Format::leaf_bits(Level, flags));
                        cursor.consume(size);
                        continue;
                    }
                    auto const below = derived().allocate_table();
                    if (below == 0) {
                        return ours::Status::NoMemory;
                    }
                    entry.value = Format::table_entry(below);
                }
                auto const status =
                    map_range<lower(Level)>(address(entry.value), cursor, flags, options);
                if (status != ours::Status::Ok) {
                    return status;
                }
            }
            return ours::Status::Ok;
        }
    }

    /// map_range() at the last level, as a tight loop: one entry a page, each
    /// made from |bits| and the next frame.
    [[nodiscard]]
    auto map_last_level(PhysAddr const table, MapCursor &cursor, PteVal const bits,
                        MapOptions const options) -> ours::Status {
        using Format = typename Derived::Format;
        auto *const entries = derived().table(table);
        for (auto at = index(PageTableLevel::PageTable, cursor.va);
             at < X86_MAX_PTES && cursor.remaining != 0; ++at) {
            auto &entry = entries[at];
            if (is_present(entry.value)) {
                if (!!(options & MapOptions::SkipIfExisting)) {
                    cursor.consume(PAGE_SIZE);
                    continue;
                }
                if (!(options & MapOptions::OverwriteIfExisting)) {
                    return ours::Status::AlreadyExists;
                }
            }
            entry.value = Format::leaf(cursor.pa(), bits);
            cursor.consume(PAGE_SIZE);
        }
        return ours::Status::Ok;
    }

    /// Whether a leaf may be written at |level|, an upper one, for a part of
    /// the range that fits one: 2 MiB or 1 GiB in a table that does not split
    /// its large leaves; in one that does, 2 MiB only, once a table is set
    /// aside for the leaf -- asked last, so that nothing is reserved for a
    /// leaf not written.
    [[nodiscard]]
    auto may_be_leaf(PageTableLevel const level, PhysAddr const owner, bool const fits) -> bool {
        if (!fits || !can_hold_leaf(level)) {
            return false;
        }
        if (!derived().splits_large_leaves()) {
            return true;
        }
        if (level != PageTableLevel::PageDirectory) {
            return false;
        }
        return derived().reserve_table(owner);
    }

    /// free_tables() below the table at |table|, at |level|, for its entries
    /// [first, last]. The depth is the number of levels, so the recursion is
    /// bounded by the hardware, not by the tree.
    auto free_below(PhysAddr const table, PageTableLevel const level, usize const first,
                    usize const last, TableTeardown &result) -> void {
        auto *const entries = derived().table(table);
        for (auto at = first; at <= last; ++at) {
            auto &entry = entries[at];
            if (!is_present(entry.value)) {
                continue;
            }
            if (is_leaf(level, entry.value)) {
                result.leaves += 1;
                continue;
            }
            auto const child = address(entry.value);
            auto const below = lower(level);
            free_below(child, below, 0, X86_MAX_PTES - 1, result);
            if (below == PageTableLevel::PageDirectory && derived().splits_large_leaves()) {
                derived().discard_all_tables(child);
            }
            entry.value = 0;
            derived().free_table(child);
            result.tables += 1;
        }
    }

    /// Splits |entry|, a 2 MiB leaf of the table at |owner|: the table set
    /// aside for it is filled with the 4 KiB leaves it stands for -- the same
    /// frames, with the same permissions -- and replaces it. Nothing is
    /// allocated, and a walker on another CPU sees the leaf or the table,
    /// never a table half filled: the fence orders the fill before the entry
    /// that publishes it.
    ///
    /// The new entries translate exactly as the leaf did, so a CPU still
    /// holding the large translation reads the same memory until the
    /// operation that follows the split changes it and invalidates.
    ///
    /// Out of line: its fill is vectorised, and inlined into a walk, the
    /// vector constants it needs were made at the top of every call of the
    /// walk, a split or not (stage 3 of docs/perf/261025/page-walk.md). A
    /// walk splits at most twice, so the call costs nothing that matters.
    [[gnu::noinline]]
    auto split(Pte &entry, PhysAddr const owner) -> void {
        using Format = typename Derived::Format;
        auto const small = derived().withdraw_table(owner);
        auto *const entries = derived().table(small);
        auto const bits =
            Format::leaf_bits(PageTableLevel::PageTable,
                              Format::to_mmu_flags(entry.value, PageTableLevel::PageDirectory));
        for (usize at = 0; at < X86_MAX_PTES; ++at) {
            entries[at].value = Format::leaf(address(entry.value) + at * PAGE_SIZE, bits);
        }
        std::atomic_thread_fence(std::memory_order_release);
        entry.value = Format::table_entry(small);
    }
};

} // namespace page_tables

#endif // #ifndef PAGE_TABLES_PAGE_TABLES_HPP
