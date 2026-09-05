/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_ARCH_VM_ASPACE_HPP
#define OURS_MM_ARCH_VM_ASPACE_HPP 1

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ours/arch/x86/page_table.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/mm/vmas_flags.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::mm {
class ArchVmAspace {
    using Self = ArchVmAspace;

  public:
    ArchVmAspace(VirtAddr base, usize size, VmAspaceKind kind);

    ~ArchVmAspace() = default;

    static auto switch_context(Self *from, Self *to) -> void;

    auto init() -> Status;

    auto map(VirtAddr, PhysAddr, usize n, MmuFlags flags, MapControl control, usize *mapped)
        -> Status;

    auto map_bulk(VirtAddr, PhysAddr *, usize n, MmuFlags flags, MapControl control, usize *mapped)
        -> Status;

    auto unmap(VirtAddr, usize n, UnmapControl control, usize *unmapped) -> Status;

    auto protect(VirtAddr, usize n, MmuFlags) -> Status;

    auto query(VirtAddr, PhysAddr *, MmuFlags *) -> Status;

    auto mark_accessed(VirtAddr, usize) -> Status {
        return Status::Unsupported;
    }

    auto harvest_accessed(VirtAddr va, usize n, HarvestControl action) -> Status;

    auto active_cpus() const -> CpuMask;

    FORCE_INLINE
    auto pgd_phys() const -> PhysAddr {
        return m_page_table.pgd_phys();
    }

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(ArchVmAspace);

    gktl::Canary m_canary;

    u16 m_pcid;
    VmAspaceKind m_kind;
    /// CPUs that are currently executing in this virtual address space.
    CpuMask m_active_cpus;
    gktl::Range<VirtAddr> m_range;
    PageTable m_page_table;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_ARCH_VM_ASPACE_HPP
