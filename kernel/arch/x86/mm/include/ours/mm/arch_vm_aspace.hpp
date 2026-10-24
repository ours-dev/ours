/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_ARCH_VM_ASPACE_HPP
#define OURS_MM_ARCH_VM_ASPACE_HPP 1

#include <ours/arch/x86/io_bitmap.hpp>
#include <ours/arch/x86/page_table.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/mm/vmas_flags.hpp>

#include <atomic>

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::mm {
class ArchVmAspace {
    using Self = ArchVmAspace;

  public:
    using Kind = VmasKind;
    using Flags = VmasFlags;
    using Teardown = PageTable::TableTeardown;

    ArchVmAspace(VirtAddr base, usize size, Kind kind);

    ~ArchVmAspace() = default;

    static auto switch_context(Self *from, Self *to) -> void;

    auto init() -> Status;

    /// Frees a user address space's page tables; see the definition. Called
    /// from ~VmAspace, never from VmAspace::destroy().
    auto destroy() -> Teardown;

    auto map(VirtAddr, PhysAddr, usize n, MmuFlags flags, MapOptions options, usize *mapped)
        -> Status;

    auto map_bulk(VirtAddr, PhysAddr *, usize n, MmuFlags flags, MapOptions options, usize *mapped)
        -> Status;

    auto unmap(VirtAddr, usize n, UnmapOptions options, usize *unmapped) -> Status;

    /// Takes permissions away and never gives one: each entry keeps only the
    /// permissions it has that are also in the flags. A permission is added by
    /// mapping the page again (unmap, then map), which names the frame.
    auto protect(VirtAddr, usize n, MmuFlags) -> Status;

    /// The frame, flags and leaf size |va| translates to; see
    /// PageTable::query_mapping().
    auto query(VirtAddr, PhysAddr *, MmuFlags *, usize *size = nullptr) -> Status;

    auto mark_accessed(VirtAddr, usize) -> Status {
        return Status::Unsupported;
    }

    auto harvest_accessed(VirtAddr va, usize n, HarvestOptions action) -> Status;

    /// The CPUs whose CR3 holds this address space's page table right now.
    /// A snapshot: by the time it is read a CPU may have switched in or out.
    auto active_cpus() const -> CpuMask;

    FORCE_INLINE
    auto pgd_phys() const -> PhysAddr {
        return m_page_table.pgd_phys();
    }

    /// Lets ring 3 use the I/O ports [base, base + count) on every thread that
    /// runs in this address space, on every CPU, once this returns; see
    /// IoBitmap::open(). The caller has checked the ports against a resource.
    /// A user address space's only: the kernel's is loaded under every kernel
    /// thread, and ring 0 is not asked about ports.
    [[nodiscard]]
    auto open_ioports(u32 base, u32 count) -> Status;

    /// Refuses those ports to ring 3 again; see IoBitmap::close().
    [[nodiscard]]
    auto close_ioports(u32 base, u32 count) -> Status;

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(ArchVmAspace);

    /// Drops every cached translation of the |n| pages at |va| that a CPU
    /// running this address space may hold, and returns once they all have:
    /// the TLB shootdown. See the definition.
    auto invalidate(VirtAddr va, usize n) -> void;

    gktl::Canary m_canary;

    u16 m_pcid;
    Kind m_kind;
    /// The CPUs whose CR3 holds this page table, and so the only ones whose
    /// TLB can hold a translation from it: a CPU that loads another root
    /// flushes every translation this one fed it (no PCID is ever allocated,
    /// and user leaves are never Global). Each CPU sets and clears its own bit,
    /// in switch_context(); anyone may read it. The same CPUs are the ones
    /// whose TSS may open |m_io_bitmap|'s ports, which is how a change to them
    /// knows whom to reload; a CPU midway through switching away, whose bit
    /// is already clear, is covered by the switch itself (switch_context()).
    std::atomic<CpuMask> m_active_cpus;
    gktl::Range<VirtAddr> m_range;
    PageTable m_page_table;
    /// The I/O ports ring 3 may use in this address space, which
    /// switch_context() loads into each CPU's TSS as it switches here.
    IoBitmap m_io_bitmap;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_ARCH_VM_ASPACE_HPP
