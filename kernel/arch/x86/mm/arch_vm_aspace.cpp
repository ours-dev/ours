#include <ours/init.hpp>
#include <ours/mm/arch_vm_aspace.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/vm_aspace.hpp>

#include <arch/x86/system.hpp>

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
///      (32GB) [0]--PDP[0]--PHYSMAP_PD[0..511]
///              |
///              |
/// KERNEL_PGD--[..]-(NULL)
///              |                [0]---PHYSMAP_PD[0][0..511]
///              |                 |
///    (512GB) [511]-PDP---PHYSMAP_PD[..][0..511]
///                                |
///                               [31]---PHYSMAP_PD[31][0..511]
///

/// In this file scope, it should be readonly.
extern "C" PhysAddr const KERNEL_PGD;

ArchVmAspace::ArchVmAspace(VirtAddr base, usize size, Kind kind)
    : m_canary(this)
    , m_pcid(INVALID_PCID)
    , m_kind(kind)
    , m_active_cpus()
    // {start, end}, which is what gktl::Range holds. This was (base, size),
    // which made the end the size; nothing read it until destroy().
    , m_range{base, base + size}
    , m_page_table() {}

auto ArchVmAspace::init() -> Status {
    m_canary.verify<Self>();
    // switch, not the if/else chain this was: the user case used to be the
    // trailing `else`, so anything that was neither Kernel nor Guest got a
    // fresh MMU page table with the kernel aliased into it -- which is right
    // for a user aspace and wrong for anything else that ever lands here.
    // Naming every kind makes a new one a compile error instead.
    switch (m_kind) {
        case VmasKind::Kernel: {
            // Kernel page table was provided by kernel.phys, so do not need to allocate memory.
            auto const phys_pgd = KERNEL_PGD;
            auto const virt_pgd = PhysMap::phys_to_virt(phys_pgd);
            auto status = m_page_table.init_mmu(phys_pgd, virt_pgd);
            if (status != Status::Ok) {
                return status;
            }
            break;
        }
        case VmasKind::Guest: {
            auto status = m_page_table.init_ept();
            if (status != Status::Ok) {
                return status;
            }
            break;
        }
        case VmasKind::User: {
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
            break;
        }
    }

    return Status::Ok;
}

/// Gives back every page-table page of a user address space: the tables
/// under its own half of the root, then the root. The frames its leaves
/// pointed at are not touched -- they belong to the VMOs that were mapped, and
/// VmAspace::destroy() has already unmapped and released those; a leaf still
/// present here is counted and reported instead.
///
/// Called from ~VmAspace, and only from there, because freeing a table is safe
/// only once no CPU can walk it:
///
///  - the dying process's last thread ran VmAspace::destroy() on these very
///    tables, so they cannot go then;
///  - its final reference is dropped later, from Thread::destroy(), a DPC that
///    runs on the same CPU after the dying thread has been switched away from
///    for the last time -- and that switch loaded another root into CR3,
///    which, with PCIDs never allocated and user leaves never Global, flushes
///    every translation and paging-structure cache entry these tables fed;
///  - a process has one thread, so no other CPU is running on them either.
///
/// The assertion below stands in for Zircon's `active_cpus_ == 0` (this tree
/// does not maintain m_active_cpus): it catches, on this CPU at least, the day
/// one of those premises stops holding.
auto ArchVmAspace::destroy() -> Teardown {
    m_canary.verify<Self>();

    if (m_kind != VmasKind::User || m_page_table.pgd_phys() == 0) {
        return {};
    }
    DEBUG_ASSERT(arch::Cr3::read().get<arch::Cr3::PageTableAddress>() != m_page_table.pgd_phys(),
                 "freeing the page tables this CPU is running on");

    return m_page_table.destroy(m_range.start, m_range.end);
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
        // Deliberately not "fall back to the kernel page table".
        //
        // Every thread carries a real address space: spawn_etc() names the
        // kernel's, and task::init() attaches it to the boot thread, which
        // predates the VMM. So a null here is not a kernel thread, it is a
        // thread nobody attached an address space to -- and quietly loading
        // the kernel page table for it would hide that, then hand ring 3 a
        // CR3 the scheduler does not believe is loaded.
        //
        // Zircon does take the fallback (X86ArchVmAspace::ContextSwitch
        // writes root_page_table_phys when aspace is null), because there
        // null is how it spells "kernel thread". We spell that explicitly
        // instead, which leaves this branch meaning only "bug".
        panic("switching to a thread with no address space");
    }

    // Finally, exchange the IO-Bitmap in TSS
}

auto ArchVmAspace::map(VirtAddr va, PhysAddr pa, usize n, MmuFlags mmuf, MapOptions options,
                       usize *mapped) -> Status {
    m_canary.verify<Self>();
    return m_page_table.map_pages(va, pa, n, mmuf, options, mapped);
}

auto ArchVmAspace::map_bulk(VirtAddr va, PhysAddr *pa, usize n, MmuFlags flags, MapOptions options,
                            usize *mapped) -> Status {
    m_canary.verify<Self>();
    return m_page_table.map_pages_bulk(va, pa, n, flags, options, mapped);
}

auto ArchVmAspace::unmap(VirtAddr va, usize n, UnmapOptions options, usize *unmapped) -> Status {
    m_canary.verify<Self>();
    return m_page_table.unmap_pages(va, n, options, unmapped);
}

auto ArchVmAspace::query(VirtAddr va, PhysAddr *pa, MmuFlags *mmuf) -> Status {
    m_canary.verify<Self>();
    return m_page_table.query_mapping(va, pa, mmuf);
}

auto ArchVmAspace::protect(VirtAddr va, usize n, MmuFlags mmuf) -> Status {
    m_canary.verify<Self>();
    return m_page_table.protect_pages(va, n, mmuf);
}

auto ArchVmAspace::restrict(VirtAddr va, usize n, MmuFlags keep) -> Status {
    m_canary.verify<Self>();
    return m_page_table.restrict_pages(va, n, keep);
}
} // namespace ours::mm
