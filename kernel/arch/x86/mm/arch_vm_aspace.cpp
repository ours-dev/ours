#include <arch/x86/system.hpp>
#include <ours/init.hpp>
#include <ours/mm/arch_vm_aspace.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/vm_aspace.hpp>

namespace ours::mm {
namespace {
    constexpr auto INVALID_PCID = u16{0xffff};
}

/// Take out 2MB-size large page mapping as the following showed:
/// +-------+---------+----------------+-------------------------+----------+-------+
/// | Level | Table  | Size  | Range   | Bits     | Entries | Pages                 |
/// +-------+--------+-----------------+-------------------------+----------+-------+
/// | 0     | (page) | -     | 2 MiB   | 21 bits  | -       | 0x1 (1)               |
/// +-------+--------+-----------------+-------------------------+----------+-------+
/// | 2     | PD     | 4 KiB | 1 GiB   | 9 bits   |   512   | 0x200 (512)           |
/// +-------+--------+-----------------+-------------------------+----------+-------+
/// | 3     | PDP    | 4 KiB | 512 GiB-| 9 bits   |   512   | 0x40000 (262144)      |
/// +-------+--------+-----------------+-------------------------+----------+-------+
/// | 4     | PML4   | 4 KiB | 256 TiB-| 9 bits   |   512   | 0x8000000 (134217728) |
/// +-------+--------+-----------------+-------------------------+----------+-------+
///
/// The layout above is kernel address space by default.
///
///      (32GB) [0]--g_pdp[0]--g_physmap_pd[0..511]
///              |
///              |
/// g_kernel_pgd--[..]-(NULL)
///              |                [0]---g_physmap_pd[0][0..511]
///              |                 |
///    (512GB) [511]-g_pdp---g_physmap_pd[..][0..511]
///                                |
///                               [31]---g_physmap_pd[31][0..511]
///

/// In this file scope, it should be readonly.
NO_MANGLE PhysAddr const g_kernel_pgd;

ArchVmAspace::ArchVmAspace(VirtAddr base, usize size, VmasFlags flags)
    : m_canary(this)
    , m_pcid(INVALID_PCID)
    , m_flags(flags)
    , m_active_cpus()
    , m_range(base, size)
    , m_page_table() {}

auto ArchVmAspace::init() -> Status {
    m_canary.verify<Self>();
    if (!!(VmasFlags::Kernel & m_flags)) [[unlikely]] {
        // Kernel page table was provided by kernel.phys, so do not need to allocate memory.
        auto const phys_pgd = g_kernel_pgd;
        auto const virt_pgd = PhysMap::phys_to_virt(phys_pgd);
        auto status = m_page_table.init_mmu(phys_pgd, virt_pgd);
        if (status != Status::Ok) {
            return status;
        }
    } else if (!!(VmasFlags::Guest & m_flags)) {
        auto status = m_page_table.init_ept();
        if (status != Status::Ok) {
            return status;
        }
    } else [[likely]] {
        auto status = m_page_table.init_mmu();
        if (status != Status::Ok) {
            return status;
        }
        // We should create an alias of kernel address space to avoid unnecessary switches.
        // And it could help us to call the routine passed by the driver of the user space correctly.
        auto &kpt = VmAspace::kernel_aspace()->arch_aspace().m_page_table;
        auto const alias_status =
            m_page_table.alias_to(kpt, PhysMap::VIRT_BASE, PhysMap::SIZE / PAGE_SIZE);
        if (alias_status != Status::Ok) {
            return alias_status;
        }
    }

    return Status::Ok;
}

auto ArchVmAspace::switch_context(Self *from, Self *to) -> void {
    using arch::Cr3;
    if (to != nullptr) {
        to->m_canary.verify<Self>();
        VirtAddr const to_pgd = to->m_page_table.pgd_phys();

        if (to->m_pcid != INVALID_PCID) {
            Cr3::read().set<Cr3::Pcid>(to->m_pcid).set<Cr3::PageTableAddress>(to_pgd).write();
        } else {
            Cr3::write(to_pgd);
        }
    } else {
        // Default to kernel aspace
        Cr3::write(g_kernel_pgd);
    }

    // Finally, exchange the IO-Bitmap in TSS
}

auto ArchVmAspace::map(VirtAddr va, PhysAddr pa, usize n, MmuFlags mmuf, MapControl control,
                       usize *mapped) -> Status {
    m_canary.verify<Self>();
    return m_page_table.map_pages(va, pa, n, mmuf, control, mapped);
}

auto ArchVmAspace::map_bulk(VirtAddr va, PhysAddr *pa, usize n, MmuFlags flags, MapControl control,
                            usize *mapped) -> Status {
    m_canary.verify<Self>();
    return m_page_table.map_pages_bulk(va, pa, n, flags, control, mapped);
}

auto ArchVmAspace::unmap(VirtAddr va, usize n, UnmapControl control, usize *unmapped) -> Status {
    m_canary.verify<Self>();
    return m_page_table.unmap_pages(va, n, control, unmapped);
}

auto ArchVmAspace::query(VirtAddr va, PhysAddr *pa, MmuFlags *mmuf) -> Status {
    m_canary.verify<Self>();
    return m_page_table.query_mapping(va, pa, mmuf);
}

auto ArchVmAspace::protect(VirtAddr va, usize n, MmuFlags mmuf) -> Status {
    m_canary.verify<Self>();
    return m_page_table.protect_pages(va, n, mmuf);
}
} // namespace ours::mm
