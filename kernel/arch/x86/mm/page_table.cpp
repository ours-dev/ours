#include <ours/arch/aspace_layout.hpp>
#include <ours/arch/x86/page_table.hpp>
#include <ours/assert.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>

#include <arch/x86/tlb.hpp>

namespace ours::mm {
namespace {
    auto phys_to_virt(PhysAddr pa) -> VirtAddr {
        return PhysMap::phys_to_virt(pa);
    }

    /// A fresh, zeroed page for one level of the table.
    ///
    /// Deliberately not alloc_frames() any more. Taking table pages straight
    /// from the buddy tied every page-table edit to a point in boot after the
    /// buddy exists -- and worse, made an edit made *while* allocating from the
    /// buddy re-enter it. alloc_page_table() picks the allocator that is up.
    auto alloc_table(usize size, usize align) -> PhysAddr {
        return alloc_page_table(size, align);
    }

    /// alloc_table()'s mirror, for PageTable::destroy().
    auto free_table(PhysAddr pa, usize size) -> void {
        free_page_table(pa, size);
    }

    constexpr auto to_status(arch::paging::MapError error) -> Status {
        switch (error) {
            case arch::paging::MapError::InvArgs:
                return Status::InvArgs;
            case arch::paging::MapError::Existent:
                return Status::AlreadyExists;
            case arch::paging::MapError::NoMem:
                return Status::NoMemory;
        }
        return Status::InternalError;
    }
} // namespace

auto PageTable::init_mmu() -> Status {
    constexpr auto TOP_LEVEL = ArchPaging::PAGING_LEVEL;

    // Through alloc_table() like every table below it, so that destroy() can
    // give all of them back through one allocator pair. It was
    // alloc_frames() directly -- the same frames today, but a root that came
    // from somewhere else than the tables it roots is one change away from
    // being freed to the wrong allocator.
    auto const phys =
        alloc_table(Engine::Traits::SIZE_OF_TABLE<TOP_LEVEL>, Engine::Traits::TABLE_ALIGNMENT);
    if (!phys) {
        return Status::NoMemory;
    }

    m_pgd_phys = phys;
    return Status::Ok;
}

auto PageTable::init_mmu(PhysAddr phys_pgd, VirtAddr virt_pgd) -> Status {
    // `virt_pgd` is redundant with PhysMap's own linear map -- phys_to_virt()
    // reaches the same address -- so it is only accepted here to keep the
    // call site symmetric with a boot-time value it already has on hand.
    (void)virt_pgd;
    m_pgd_phys = phys_pgd;
    return Status::Ok;
}

auto PageTable::init_ept() -> Status {
    return Status::NotSupported;
}

auto PageTable::alias_to(Self &other, VirtAddr base, usize n) -> Status {
    constexpr auto TOP_LEVEL = ArchPaging::PAGING_LEVEL;
    using TopTable = Engine::Table<TOP_LEVEL>;

    auto &src = *reinterpret_cast<TopTable *>(phys_to_virt(other.m_pgd_phys));
    auto &dst = *reinterpret_cast<TopTable *>(phys_to_virt(m_pgd_phys));

    auto const start_idx = Engine::Traits::virt_to_index(TOP_LEVEL, base);
    auto const end_idx = Engine::Traits::virt_to_index(TOP_LEVEL, base + (n - 1) * PAGE_SIZE);

    // A shallow copy of the top-level entries that cover [base, base + n):
    // this address space and `other` end up sharing the same lower-level
    // tables for that range, so a mapping either one makes later is visible
    // to both -- exactly what aliasing the kernel range into every new
    // address space needs, without walking or duplicating anything below.
    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    for (auto idx = start_idx; idx <= end_idx; ++idx) {
        dst[idx] = src[idx];
    }

    return Status::Ok;
}

auto PageTable::map_pages(VirtAddr va, PhysAddr pa, usize n, MmuFlags flags, MapOptions options,
                          usize *mapped) -> Status {
    // The engine always rejects a page that is already mapped; there is no
    // overwrite/skip mode to honor yet, so `options` goes unused for now.
    (void)options;

    auto result = [&] {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        return Engine::map(m_pgd_phys, phys_to_virt, alloc_table, va, pa, n, flags);
    }();

    if (result.is_err()) {
        if (mapped) {
            *mapped = 0;
        }
        return to_status(result.error());
    }
    if (mapped) {
        *mapped = n;
    }
    return Status::Ok;
}

auto PageTable::map_pages_bulk(VirtAddr va, PhysAddr *pa, usize n, MmuFlags flags,
                               MapOptions options, usize *mapped) -> Status {
    (void)options;

    usize done = 0;
    Status status = Status::Ok;
    {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        for (usize i = 0; i < n; ++i) {
            auto result = Engine::map(m_pgd_phys, phys_to_virt, alloc_table, va + i * PAGE_SIZE,
                                      pa[i], 1, flags);
            if (result.is_err()) {
                status = to_status(result.error());
                break;
            }
            ++done;
        }
    }

    if (mapped) {
        *mapped = done;
    }
    return status;
}

auto PageTable::unmap_pages(VirtAddr va, usize n, UnmapOptions options, usize *unmapped) -> Status {
    (void)options;

    auto result = [&] {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        return Engine::unmap(m_pgd_phys, phys_to_virt, va, n);
    }();

    if (result.is_err()) {
        if (unmapped) {
            *unmapped = 0;
        }
        return to_status(result.error());
    }

    auto const pages = result.unwrap();
    for (usize i = 0; i < pages; ++i) {
        arch::x86_invlpg(va + i * PAGE_SIZE);
    }
    if (unmapped) {
        *unmapped = pages;
    }
    return Status::Ok;
}

auto PageTable::protect_pages(VirtAddr va, usize n, MmuFlags flags) -> Status {
    auto result = [&] {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        return Engine::protect(m_pgd_phys, phys_to_virt, va, n, flags);
    }();

    if (result.is_err()) {
        return to_status(result.error());
    }

    auto const pages = result.unwrap();
    for (usize i = 0; i < pages; ++i) {
        arch::x86_invlpg(va + i * PAGE_SIZE);
    }
    return Status::Ok;
}

auto PageTable::restrict_pages(VirtAddr va, usize n, MmuFlags keep) -> Status {
    auto result = [&] {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        return Engine::restrict(m_pgd_phys, phys_to_virt, va, n, keep);
    }();

    if (result.is_err()) {
        return to_status(result.error());
    }

    // The local TLB only, as protect_pages() flushes it. What another CPU
    // running this address space still holds is the open question of the
    // clone's design: an entry that had Write and lost it here can still be
    // written through there until that CPU's TLB is flushed.
    auto const pages = result.unwrap();
    for (usize i = 0; i < pages; ++i) {
        arch::x86_invlpg(va + i * PAGE_SIZE);
    }
    return Status::Ok;
}

auto PageTable::query_mapping(VirtAddr va, PhysAddr *pa, MmuFlags *flags) -> Status {
    auto result = [&] {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        return Engine::query(m_pgd_phys, phys_to_virt, va);
    }();

    if (result.is_none()) {
        return Status::NotFound;
    }
    auto const &mapping = result.value();
    if (pa) {
        *pa = mapping.phys;
    }
    if (flags) {
        *flags = mapping.flags;
    }
    return Status::Ok;
}

/// Frees the tables under the top-level entries covering [base, end) -- a
/// user address space's own half -- and then the root table itself.
///
/// Only for a table no CPU has loaded, since nothing here flushes a TLB: the
/// caller is ~VmAspace, which runs once the last reference to the address
/// space is gone, and that is after its last thread was switched away from
/// for good (ArchVmAspace::destroy() says why that holds).
///
/// The range must stop short of the kernel's top-level entries. alias_to()
/// copied those in from the kernel's own root, so the tables under them are
/// the kernel's, shared by every address space: freeing them would pull the
/// kernel out from under every CPU. Checked, not assumed.
auto PageTable::destroy(VirtAddr const base, VirtAddr const end) -> TableTeardown {
    constexpr auto TOP_LEVEL = ArchPaging::PAGING_LEVEL;
    DEBUG_ASSERT(m_pgd_phys != 0, "destroying a page table that has no root");
    DEBUG_ASSERT(base < end, "an empty range to destroy");

    auto const first = Engine::Traits::virt_to_index(TOP_LEVEL, base);
    auto const last = Engine::Traits::virt_to_index(TOP_LEVEL, end - 1);
    ASSERT(last < Engine::Traits::virt_to_index(TOP_LEVEL, KERNEL_ASPACE_BASE),
           "top-level entries {}..{} reach the kernel's, which are shared", first, last);

    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    auto result = Engine::free_tables(m_pgd_phys, phys_to_virt, free_table, first, last);
    free_table(m_pgd_phys, Engine::Traits::SIZE_OF_TABLE<TOP_LEVEL>);
    result.tables += 1;
    m_pgd_phys = 0;
    return result;
}

} // namespace ours::mm
