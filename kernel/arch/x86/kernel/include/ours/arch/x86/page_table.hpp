/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_PAGE_TABLE_HPP
#define OURS_ARCH_X86_PAGE_TABLE_HPP 1

#include <ours/mm/page_table_deposits.hpp>
#include <ours/mm/types.hpp>
#include <ours/status.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/types.hpp>

#include <arch/paging.hpp>
#include <arch/paging/arch_mmu_flags.hpp>
#include <arch/paging/x86_pagings.hpp>
#include <page_tables/page_tables.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::mm {

/// x86's `ArchVmAspace::m_page_table`: owns one hardware page-table tree
/// (a plain PhysAddr to its root, nothing more) and the lock over it, and is
/// everything ArchVmAspace needs on top: init, aliasing the kernel range into
/// a new address space, and the mapping operations.
///
/// The walks are x86's own, in the base this class derives from
/// (page_tables::X86PageTableImpl), which asks it for the root, how a table
/// is reached (PhysMap) and what a leaf's bits mean. They move there from
/// the generic paging engine (`arch::paging::Paging<X86Paging<...>>`) one
/// operation at a time; query is there, the others still drive the engine.
///
/// Only 4-level (PML4) paging for now; LA57 is not wired up (see
/// //kernel/arch/x86/mm/mmu.cpp's `#if PAGING_LEVEL == 5`, which nothing
/// currently defines).
class PageTable final: public ::page_tables::X86PageTableImpl<PageTable> {
    friend class ::page_tables::X86PageTableImpl<PageTable>;
    using Self = PageTable;
    using ArchPaging =
        ::arch::paging::PagingDispatcher<::arch::paging::X86PagingLevel::PageMapLevel4>::Type;
    using Engine = ::arch::paging::Paging<ArchPaging>;

  public:
    using TableTeardown = Engine::TableTeardown;

    /// What kind of table this is, fixed when it is made.
    enum class Flags : u8 {
        None = 0,

        /// Its 2 MiB leaves are mapped with a table set aside for each and are
        /// split into it when a change covers part of one
        /// (PageTableDeposits): a user address space's, whose mappings change
        /// in part. The kernel's are not: its large leaves are boot's, never
        /// changed in part (decision D5 of .agents/tasks/large-pages).
        SplitsLargeLeaves = 0_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    PageTable() = default;
    ~PageTable() = default;

    USTL_NO_MOVEABLE_AND_COPYABLE(PageTable);

    /// Allocate a fresh root table for a brand-new address space, of the kind
    /// |flags| says.
    auto init_mmu(Flags flags) -> Status;

    /// Adopt a root table that already exists (the kernel's, handed down
    /// from kernel.phys) instead of allocating one.
    auto init_mmu(PhysAddr phys_pgd, VirtAddr virt_pgd) -> Status;

    /// Second-level (EPT) translation for guest address spaces. Not
    /// implemented yet -- always returns Status::NotSupported.
    auto init_ept() -> Status;

    /// Map `n` pages of `other`'s translation, starting at `base`, into this
    /// table at the same virtual range. Used to give every non-kernel
    /// address space the kernel's own mappings without switching CR3 on
    /// every kernel/user transition.
    auto alias_to(Self &other, VirtAddr base, usize n) -> Status;

    FORCE_INLINE
    auto pgd_phys() const -> PhysAddr {
        return m_pgd_phys;
    }

    auto map_pages(VirtAddr va, PhysAddr pa, usize n, MmuFlags flags, ArchMapOptions options,
                   usize *mapped) -> Status;

    auto map_pages_bulk(VirtAddr va, PhysAddr *pa, usize n, MmuFlags flags, ArchMapOptions options,
                        usize *mapped) -> Status;

    auto unmap_pages(VirtAddr va, usize n, UnmapOptions options, usize *unmapped) -> Status;

    /// Takes permissions away and never gives one; see Engine::protect().
    auto protect_pages(VirtAddr va, usize n, MmuFlags flags) -> Status;

    /// Whether the 2 MiB entry |va| falls under is empty, so that a 2 MiB leaf
    /// could be written there; see Engine::second_level_empty().
    [[nodiscard]]
    auto second_level_empty(VirtAddr va) -> bool;

    /// What |va| translates to: its frame, its flags, and the size of the leaf
    /// that maps it, each written when its pointer is not null.
    auto query_mapping(VirtAddr va, PhysAddr *pa, MmuFlags *flags, usize *size = nullptr) -> Status;

    /// Frees every table under the top-level entries covering [base, end) and
    /// then the root table, leaving this object with no table at all; see
    /// the definition for when that is safe.
    auto destroy(VirtAddr base, VirtAddr end) -> TableTeardown;

  private:
    /// Calls |op| with the deposits this table's kind uses: PageTableDeposits
    /// for one that splits its large leaves, NoDeposits otherwise. The one
    /// place the choice is made, so no operation can make it differently.
    template <typename Op>
    auto with_deposits(Op &&op) {
        if (!!(m_flags & Flags::SplitsLargeLeaves)) {
            PageTableDeposits deposits;
            return op(deposits);
        }
        ::arch::paging::NoDeposits none;
        return op(none);
    }

    // What the walks of X86PageTableImpl ask of this kind of table.
    static constexpr auto top_level() -> ::page_tables::PageTableLevel {
        return ::page_tables::PageTableLevel::PageMapLevel4;
    }

    [[nodiscard]]
    auto root() const -> PhysAddr {
        return m_pgd_phys;
    }

    /// The table at |pa|, through PhysMap.
    [[nodiscard]]
    static auto table(PhysAddr pa) -> ::arch::PteVal *;

    /// What a leaf's bits grant, in the neutral flags.
    [[nodiscard]]
    static auto to_mmu_flags(::arch::PteVal entry, ::page_tables::PageTableLevel level) -> MmuFlags;

    /// Held with interrupts off. Its holders include the page fault path,
    /// which reaches it from the #PF handler with interrupts still off, and
    /// init_vmm(), which re-protects the kernel image's mappings before
    /// InitLevel::Threading gives the boot CPU a thread whose preemption
    /// could be held off. Holding it with interrupts on anywhere else would
    /// let a holder be preempted, and the next thread on this CPU to map or
    /// fault in the same address space spin on a lock its own CPU holds.
    sync::SpinLock m_lock;
    PhysAddr m_pgd_phys = 0;
    Flags m_flags = Flags::None;
};

} // namespace ours::mm

#endif // #ifndef OURS_ARCH_X86_PAGE_TABLE_HPP
