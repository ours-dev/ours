/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PAGE_TABLES_PAGE_TABLES_HPP
#define PAGE_TABLES_PAGE_TABLES_HPP 1

#include <atomic>

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

/// What a virtual address translates to: the physical address it reaches,
/// the size of the leaf that maps it, and that leaf's permissions.
struct Mapping {
    PhysAddr phys;
    usize size;
    MmuFlags flags;
};

/// Where a walk over a range is: the next address, and the bytes still to do.
struct RangeCursor {
    VirtAddr va;
    usize remaining;
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
///   - `table(PhysAddr) -> PteVal *`: where the table at a physical address
///     is seen from the address space the walk runs in;
///   - `splits_large_leaves() -> bool`: whether its 2 MiB leaves each have a
///     table set aside, so that a change to part of one splits it;
///   - `withdraw_table(PhysAddr owner) -> PhysAddr`: takes back a table set
///     aside with |owner|, the table holding the leaf, to split the leaf;
///   - `discard_table(PhysAddr owner)`: frees one of |owner|'s, when a whole
///     2 MiB leaf goes.
///
/// The level is an argument, not a template parameter, so that each walk is
/// emitted once for all levels rather than once per level; so is whether
/// large leaves split, which is one question asked where a split may happen.
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
            auto const entry = derived().table(table)[index(level, va)];
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
        auto clear = [this](PteVal &entry, PageTableLevel const level, PhysAddr const owner) {
            entry = 0;
            if (level == PageTableLevel::PageDirectory && derived().splits_large_leaves()) {
                derived().discard_table(owner);
            }
        };
        return walk_range(derived().root(), derived().top_level(), cursor, clear);
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
        auto narrow = [flags](PteVal &entry, PageTableLevel const level, PhysAddr) {
            auto const current = Format::to_mmu_flags(entry, level);
            auto const kept =
                (current & ~MmuFlags::PermMask) | (current & flags & MmuFlags::PermMask);
            entry = Format::with_permissions(entry, level, kept);
        };
        return walk_range(derived().root(), derived().top_level(), cursor, narrow);
    }

  private:
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

    /// Walks the range at |cursor| below the table at |table|, at |level|,
    /// once, table by table: each table on the way is entered once, and its
    /// entries are stepped through in a loop up to its end or the range's.
    /// For each entry:
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
    template <typename Action>
    [[nodiscard]]
    auto walk_range(PhysAddr const table, PageTableLevel const level, RangeCursor &cursor,
                    Action &action) -> ours::Status {
        if (level == PageTableLevel::PageTable) {
            walk_last_level(table, cursor, action);
            return ours::Status::Ok;
        }
        auto *const entries = derived().table(table);
        auto const size = page_size(level);
        for (auto at = index(level, cursor.va); at < X86_MAX_PTES && cursor.remaining != 0; ++at) {
            auto &entry = entries[at];
            // The part of the range under this entry.
            auto const offset = cursor.va & (size - 1);
            auto step = size - offset;
            if (step > cursor.remaining) {
                step = cursor.remaining;
            }

            if (!is_present(entry)) {
                cursor.va += step;
                cursor.remaining -= step;
                continue;
            }
            if (is_leaf(level, entry)) {
                if (offset == 0 && step == size) {
                    action(entry, level, table);
                    cursor.va += step;
                    cursor.remaining -= step;
                    continue;
                }
                if (level != PageTableLevel::PageDirectory || !derived().splits_large_leaves()) {
                    return ours::Status::InvArgs;
                }
                split(entry, table);
            }
            auto const status = walk_range(address(entry), lower(level), cursor, action);
            if (status != ours::Status::Ok) {
                return status;
            }
        }
        return ours::Status::Ok;
    }

    /// walk_range() at the last level, as a tight loop: every present entry
    /// there is a leaf of one page, covered whole, so none of the upper
    /// levels' questions arise.
    template <typename Action>
    auto walk_last_level(PhysAddr const table, RangeCursor &cursor, Action &action) -> void {
        auto *const entries = derived().table(table);
        for (auto at = index(PageTableLevel::PageTable, cursor.va);
             at < X86_MAX_PTES && cursor.remaining != 0; ++at) {
            if (is_present(entries[at])) {
                action(entries[at], PageTableLevel::PageTable, table);
            }
            cursor.va += PAGE_SIZE;
            cursor.remaining -= PAGE_SIZE;
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
    auto split(PteVal &entry, PhysAddr const owner) -> void {
        using Format = typename Derived::Format;
        auto const small = derived().withdraw_table(owner);
        auto *const entries = derived().table(small);
        auto const flags = Format::to_mmu_flags(entry, PageTableLevel::PageDirectory);
        for (usize at = 0; at < X86_MAX_PTES; ++at) {
            entries[at] =
                Format::leaf(PageTableLevel::PageTable, address(entry) + at * PAGE_SIZE, flags);
        }
        std::atomic_thread_fence(std::memory_order_release);
        entry = Format::table_entry(small);
    }
};

} // namespace page_tables

#endif // #ifndef PAGE_TABLES_PAGE_TABLES_HPP
