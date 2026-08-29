/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_PAGE_TABLE_HPP
#define OURS_ARCH_X86_PAGE_TABLE_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/mm/types.hpp>

#include <ours/sync/spin_lock.hpp>

#include <arch/paging.hpp>
#include <arch/paging/arch_mmu_flags.hpp>
#include <arch/paging/x86_pagings.hpp>

namespace ours::mm {

/// x86's `ArchVmAspace::m_page_table`: owns one hardware page-table tree
/// (a plain PhysAddr to its root, nothing more) and drives it through
/// `arch::paging::Paging<X86Paging<...>>` -- a stateless walker that only
/// knows how to read/write PTEs given a root, a phys-to-virt view and a
/// page-frame allocator. This class supplies those three things (backed by
/// the PMM and PhysMap) and is everything ArchVmAspace needs on top: init,
/// aliasing the kernel range into a new address space, and the four
/// mapping operations.
///
/// Only 4-level (PML4) paging for now; LA57 is not wired up (see
/// //kernel/arch/x86/mm/mmu.cpp's `#if PAGING_LEVEL == 5`, which nothing
/// currently defines).
class PageTable {
    using Self = PageTable;
    using ArchPaging = ::arch::paging::PagingDispatcher<::arch::paging::X86PagingLevel::PageMapLevel4>::Type;
    using Engine = ::arch::paging::Paging<ArchPaging>;

  public:
    PageTable() = default;
    ~PageTable() = default;

    USTL_NO_MOVEABLE_AND_COPYABLE(PageTable);

    /// Allocate a fresh root table for a brand-new address space.
    auto init_mmu() -> Status;

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

    auto map_pages(VirtAddr va, PhysAddr pa, usize n, MmuFlags flags, MapControl control,
                   usize *mapped) -> Status;

    auto map_pages_bulk(VirtAddr va, PhysAddr *pa, usize n, MmuFlags flags, MapControl control,
                        usize *mapped) -> Status;

    auto unmap_pages(VirtAddr va, usize n, UnmapControl control, usize *unmapped) -> Status;

    auto protect_pages(VirtAddr va, usize n, MmuFlags flags) -> Status;

    auto query_mapping(VirtAddr va, PhysAddr *pa, MmuFlags *flags) -> Status;

  private:
    sync::SpinLock m_lock;
    PhysAddr m_pgd_phys = 0;
};

} // namespace ours::mm

#endif // #ifndef OURS_ARCH_X86_PAGE_TABLE_HPP
