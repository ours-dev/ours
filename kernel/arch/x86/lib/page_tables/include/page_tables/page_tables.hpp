/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PAGE_TABLES_PAGE_TABLES_HPP
#define PAGE_TABLES_PAGE_TABLES_HPP 1

#include <arch/paging/mmu_flags.hpp>
#include <arch/paging/x86_pagings.hpp>
#include <arch/types.hpp>
#include <arch/x86/macro/mmu.hpp>
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

/// The walks of an x86 page table, written once for every kind of table that
/// derives from it (`class Mmu: public X86PageTableImpl<Mmu>`). The walks
/// hold no state and take no lock: the derived class owns the root and the
/// lock, and calls them under it, so they are protected.
///
/// |Derived| supplies what differs between kinds of table, and the walks ask
/// it through `derived()`, resolved at compile time:
///
///   - `top_level() -> PageTableLevel`: the level of its root table;
///   - `root() -> PhysAddr`: its root table;
///   - `table(PhysAddr) -> PteVal *`: where the table at a physical address
///     is seen from the address space the walk runs in;
///   - `to_mmu_flags(PteVal, PageTableLevel) -> MmuFlags`: what a leaf's
///     bits grant.
///
/// The level is an argument, not a template parameter, so that each walk is
/// emitted once for all levels rather than once per level.
template <typename Derived>
class X86PageTableImpl {
  protected:
    /// What |va| translates to, or none when nothing maps it.
    [[nodiscard]]
    auto query(VirtAddr const va) const -> ustl::Option<Mapping> {
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
                    .flags = derived().to_mmu_flags(entry, level),
                });
            }
            table = address(entry);
            level = lower(level);
        }
    }

  private:
    [[nodiscard]]
    auto derived() const -> Derived const & {
        return static_cast<Derived const &>(*this);
    }
};

} // namespace page_tables

#endif // #ifndef PAGE_TABLES_PAGE_TABLES_HPP
