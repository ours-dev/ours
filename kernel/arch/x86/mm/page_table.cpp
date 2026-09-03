#include <arch/x86/tlb.hpp>
#include <ours/arch/x86/page_table.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>

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

    constexpr auto to_status(arch::paging::MapError error) -> Status {
        switch (error) {
            case arch::paging::MapError::InvalidArgument:
                return Status::InvalidArgument;
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
    constexpr auto ORDER = get_order(Engine::Traits::SIZE_OF_TABLE<TOP_LEVEL> / PAGE_SIZE);

    PhysAddr phys;
    auto *frame = alloc_frames(GAF_KERNEL | Gaf::Zero, &phys, ORDER);
    if (!frame) {
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
    Guard<sync::SpinLock> guard(m_lock);
    for (auto idx = start_idx; idx <= end_idx; ++idx) {
        dst[idx] = src[idx];
    }

    return Status::Ok;
}

auto PageTable::map_pages(VirtAddr va, PhysAddr pa, usize n, MmuFlags flags, MapControl control,
                          usize *mapped) -> Status {
    // The engine always rejects a page that is already mapped; there is no
    // overwrite/skip mode to honor yet, so `control` goes unused for now.
    (void)control;

    auto result = [&] {
        Guard<sync::SpinLock> guard(m_lock);
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
                               MapControl control, usize *mapped) -> Status {
    (void)control;

    usize done = 0;
    Status status = Status::Ok;
    {
        Guard<sync::SpinLock> guard(m_lock);
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

auto PageTable::unmap_pages(VirtAddr va, usize n, UnmapControl control, usize *unmapped) -> Status {
    (void)control;

    auto result = [&] {
        Guard<sync::SpinLock> guard(m_lock);
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
        Guard<sync::SpinLock> guard(m_lock);
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

auto PageTable::query_mapping(VirtAddr va, PhysAddr *pa, MmuFlags *flags) -> Status {
    auto result = [&] {
        Guard<sync::SpinLock> guard(m_lock);
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

} // namespace ours::mm
