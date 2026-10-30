/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PAGE_TABLES_MMU_FORMAT_HPP
#define PAGE_TABLES_MMU_FORMAT_HPP 1

#include <arch/paging/arch_mmu_flags.hpp>
#include <arch/paging/mmu_flags.hpp>
#include <arch/paging/x86_pagings.hpp>
#include <arch/types.hpp>
#include <arch/x86/macro/mmu.hpp>

namespace page_tables {

/// What the bits of an x86 MMU page table's entries mean, for the walks of
/// X86PageTableImpl: a derived class names it as its `Format`. Another kind
/// of table (a guest's second-level table) would name another format, with
/// the same functions and its own bits.
///
/// The bits are the ones `arch::paging::X86Pte` makes, so that this and the
/// boot path's tables agree on what an entry is: which bits a leaf gets
/// besides its permissions (Accessed, Dirty when writable, Global when not
/// User) is written down once, there.
struct MmuFormat {
    using X86MmuFlags = arch::paging::X86MmuFlags;
    using Conv = arch::paging::MmuFlagsConv<X86MmuFlags>;
    using Level = arch::paging::X86PagingLevel;

    /// The bit the hardware sets in a leaf when it loads the leaf's translation
    /// into a TLB, and never clears; harvest_accessed() clears it.
    static constexpr arch::PteVal ACCESSED = X86_MMUF_ACCESSED;

    /// What a leaf at |level| grants, in the neutral flags.
    [[nodiscard]]
    static constexpr auto to_mmu_flags(arch::PteVal const entry, Level) -> arch::paging::MmuFlags {
        return Conv::from_arch(X86MmuFlags(entry & X86_MMUF_MASK));
    }

    /// Every bit of a leaf at |level| granting |flags| but its address: the
    /// part a map works out once and gives every frame it maps. Above the
    /// last level it carries the page-size bit, which is what makes it a
    /// leaf there.
    [[nodiscard]]
    static constexpr auto leaf_bits(Level const level, arch::paging::MmuFlags const flags)
        -> arch::PteVal {
        auto value =
            arch::paging::X86Pte<Level::PageTable>::make(0, Conv::to_arch(flags), true).inner_;
        if (level != Level::PageTable) {
            value |= X86_MMUF_PAGE_SIZE;
        }
        return value;
    }

    /// A leaf mapping |pa|, with the bits leaf_bits() gave.
    [[nodiscard]]
    static constexpr auto leaf(arch::PhysAddr const pa, arch::PteVal const bits) -> arch::PteVal {
        return (pa & X86_PG_FRAME) | bits;
    }

    /// An entry pointing to the table at |pa|. It grants everything, so that
    /// only the leaves under it decide.
    [[nodiscard]]
    static constexpr auto table_entry(arch::PhysAddr const pa) -> arch::PteVal {
        return arch::paging::X86Pte<Level::PageTable>::make(pa, X86MmuFlags{}, false).inner_;
    }

    /// What taking permissions away does to every entry of a range, worked
    /// out once from the permissions to keep: a permission |flags| lacks is
    /// cleared (Writable) or set (no-execute, since x86 states the lack of
    /// Executable), and nothing else changes, so the same two masks serve
    /// every entry. Done per entry instead, from the entry's bits to the
    /// neutral flags, ANDed with |flags|, and back, the work is about twice
    /// the instructions a page (docs/perf/261025/page-walk.md, stage 5).
    struct Narrowing {
        arch::PteVal keep;
        arch::PteVal set;
    };

    [[nodiscard]]
    static constexpr auto narrowing(arch::paging::MmuFlags const flags) -> Narrowing {
        Narrowing narrowing{.keep = ~arch::PteVal{0}, .set = 0};
        if (!(flags & arch::paging::MmuFlags::Writable)) {
            narrowing.keep &= ~arch::PteVal{X86_MMUF_WRITABLE};
        }
        if (!(flags & arch::paging::MmuFlags::Executable)) {
            narrowing.set |= arch::PteVal{X86_MMUF_NOEXECUTABLE};
        }
        return narrowing;
    }

    /// |entry| with the permissions |narrowing| takes away, and every other
    /// bit as it was.
    [[nodiscard]]
    static constexpr auto narrow(arch::PteVal const entry, Narrowing const narrowing)
        -> arch::PteVal {
        return (entry & narrowing.keep) | narrowing.set;
    }
};

} // namespace page_tables

#endif // #ifndef PAGE_TABLES_MMU_FORMAT_HPP
