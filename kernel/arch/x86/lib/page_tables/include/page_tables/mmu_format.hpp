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

    /// |entry| with its permission bits (Writable, User, cache-disable,
    /// no-execute) replaced by those |flags| grant, and every other bit as
    /// it was.
    [[nodiscard]]
    static constexpr auto with_permissions(arch::PteVal const entry, Level,
                                           arch::paging::MmuFlags const flags) -> arch::PteVal {
        return arch::paging::X86Pte<Level::PageTable>{entry}
            .with_protection(Conv::to_arch(flags))
            .inner_;
    }
};

} // namespace page_tables

#endif // #ifndef PAGE_TABLES_MMU_FORMAT_HPP
