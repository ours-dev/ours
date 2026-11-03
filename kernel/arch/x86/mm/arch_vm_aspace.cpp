#include <ours/cpu.hpp>
#include <ours/init.hpp>
#include <ours/ipi_service.hpp>
#include <ours/mm/arch_vm_aspace.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/vm_aspace.hpp>

#include <utility>

#include <arch/x86/system.hpp>
#include <arch/x86/tlb.hpp>

namespace ours::mm {
namespace {
    constexpr auto INVALID_PCID = u16{0xffff};

    /// Sets or clears |cpu|'s bit in |mask|. std::atomic<CpuMask> has no
    /// fetch_or or fetch_and, so it is a compare-and-swap loop, as
    /// IpiService::handle_generic() clears its own bit: two CPUs switching at
    /// once would otherwise each store a mask that lacks the other's change.
    auto set_cpu(std::atomic<CpuMask> &mask, CpuNum const cpu, bool const value) -> void {
        auto expected = mask.load(std::memory_order_relaxed);
        for (;;) {
            auto desired = expected;
            desired.set(cpu, value);
            if (mask.compare_exchange_weak(expected, desired, std::memory_order_seq_cst,
                                           std::memory_order_relaxed)) {
                return;
            }
        }
    }
} // namespace

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
            auto status = m_page_table.init_mmu(PageTable::Flags::SplitsLargeLeaves);
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
/// The assertions below say that no CPU has these tables loaded, this one or
/// any other: CR3 here does not hold them and the active mask is empty. They
/// catch the day one of those premises stops holding.
auto ArchVmAspace::destroy() -> Teardown {
    m_canary.verify<Self>();

    if (m_kind != VmasKind::User || m_page_table.pgd_phys() == PhysAddr{0}) {
        return {};
    }
    DEBUG_ASSERT(arch::Cr3::read().get<arch::Cr3::PageTableAddress>() !=
                     std::to_underlying(m_page_table.pgd_phys()),
                 "freeing the page tables this CPU is running on");
    DEBUG_ASSERT(active_cpus().none(), "freeing page tables another CPU is running on");

    return m_page_table.destroy(m_range.start, m_range.end);
}

/// Loads |to|'s page table on this CPU and moves this CPU's bit in the active
/// masks from |from| to |to|. The caller keeps this thread on this CPU across
/// the call: the scheduler holds its lock, and a test that attaches itself to
/// an address space disables preemption.
///
/// The order is what makes the mask safe to read for a TLB shootdown:
///
///  - |to|'s bit is set before CR3 is written, by a sequentially consistent
///    read-modify-write, and the write to CR3 is serialising. So by the time
///    this CPU can walk |to|'s tables and cache a translation from them, the
///    bit is visible to every CPU. One that changes an entry and then reads the
///    mask either sees the bit, and invalidates here, or changed the entry
///    before this CPU walked it, and this CPU caches the new entry.
///  - |from|'s bit is cleared after CR3 is written, once nothing |from| fed
///    this CPU can be left in its TLB. Clearing it earlier would let a
///    shootdown skip a CPU that still holds |from|'s translations.
///
/// Clearing the outgoing bit first, before the incoming one is set, would be
/// safe without PCIDs as well -- a shootdown that skips this CPU in between is
/// made good by the CR3 write that follows, and the switch touches no user page
/// before it -- but clearing it last needs no such argument.
///
/// The I/O bitmap goes last, and needs only half of that order: |to|'s bit is
/// set before IoBitmap::load() reads |to|'s ports, so a change to them either
/// is seen by the load or reloads this CPU afterwards. Clearing |from|'s bit
/// before its ports are closed is safe without the other half: a change made
/// before the close is caught by the version check there, which then closes
/// every port, and one made after it finds nothing of |from|'s left here.
auto ArchVmAspace::switch_context(Self *from, Self *to) -> void {
    using arch::Cr3;
    if (to != nullptr) {
        to->m_canary.verify<Self>();
        PhysAddr const to_pgd = to->m_page_table.pgd_phys();
        auto const cpu = Cpu::id();
        set_cpu(to->m_active_cpus, cpu, true);

        if (to->m_pcid != INVALID_PCID) {
            Cr3::read()
                .set<Cr3::Pcid>(to->m_pcid)
                .set<Cr3::PageTableAddress>(std::to_underlying(to_pgd))
                .write();
        } else {
            Cr3::write(to_pgd);
        }

        // Not when |from| is |to|: that would clear the bit just set, on a CPU
        // that is running on these tables.
        if (from != nullptr && from != to) {
            from->m_canary.verify<Self>();
            set_cpu(from->m_active_cpus, cpu, false);
        }

        // The ports this CPU's TSS opens follow the address space: those of
        // the one it ran before close, |to|'s open.
        IoBitmap::load(to->m_io_bitmap);
    } else {
        // Deliberately not "fall back to the kernel page table".
        //
        // Every thread carries a real address space: spawn_etc() names the
        // kernel's, and task::init() attaches it to the boot thread, which
        // predates the VMM. So a null here is not a kernel thread, it is a
        // thread nobody attached an address space to -- and quietly loading
        // the kernel page table for it would hide that, then hand ring 3 a
        // CR3 the scheduler does not believe is loaded. Since a kernel thread
        // names the kernel's address space explicitly, this branch means only
        // "bug".
        panic("switching to a thread with no address space");
    }
}

/// Two maps change translations a CPU may hold, and are invalidated over the
/// whole range before this returns:
///
///  - one with OverwriteIfExisting, whatever its outcome: an entry written
///    over may be cached on any CPU running this address space. Only after
///    the invalidation may the caller let the old frame go to anyone else.
///    x86 lets a present entry be changed in place; an architecture that
///    needs the old entry cleared and invalidated before the new one is
///    written (arm64's break-before-make) would do that in its page table.
///  - one that failed after UndoOnFailure: it wrote entries and took them
///    away again, and a CPU may have cached one in between.
///
/// The whole range, not the entries that changed: the page table does not
/// say which they were (docs/repairs/261025/arch-paging.md, items 3 and 4).
///
/// The kernel never asks for anything but one choice of each group of the
/// options; the page table would refuse it with InvArgs, but a caller that
/// asks has a bug.
auto ArchVmAspace::map(VirtAddr va, PhysAddr pa, usize n, MmuFlags mmuf, ArchMapOptions options,
                       usize *mapped) -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(map_options_valid(options), "map options {:#x} are not one of each group",
                 std::to_underlying(options));
    auto const status = m_page_table.map_pages(va, pa, n, mmuf, options, mapped);
    if (!!(options & ArchMapOptions::OverwriteIfExisting) ||
        (Status::Ok != status && !!(options & ArchMapOptions::UndoOnFailure))) {
        invalidate(va, n);
    }
    return status;
}

/// As map().
auto ArchVmAspace::map_bulk(VirtAddr va, PhysAddr *pa, usize n, MmuFlags flags,
                            ArchMapOptions options, usize *mapped) -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(map_options_valid(options), "map options {:#x} are not one of each group",
                 std::to_underlying(options));
    auto const status = m_page_table.map_pages_bulk(va, pa, n, flags, options, mapped);
    if (!!(options & ArchMapOptions::OverwriteIfExisting) ||
        (Status::Ok != status && !!(options & ArchMapOptions::UndoOnFailure))) {
        invalidate(va, n);
    }
    return status;
}

auto ArchVmAspace::unmap(VirtAddr va, usize n, UnmapOptions options, usize *unmapped) -> Status {
    m_canary.verify<Self>();
    usize done = 0;
    auto const status = m_page_table.unmap_pages(va, n, options, &done);
    if (status == Status::Ok) {
        invalidate(va, done);
    }
    if (unmapped) {
        *unmapped = done;
    }
    return status;
}

auto ArchVmAspace::harvest_accessed(VirtAddr const va, usize const n, HarvestOptions const action,
                                    AccessedVisitor const visit, void *const context,
                                    usize *const cleared) -> Status {
    m_canary.verify<Self>();
    *cleared = 0;
    if (!!(action & HarvestOptions::ZapUnaccessed)) {
        return Status::NotSupported;
    }
    if (!(action & HarvestOptions::UpdateAge)) {
        return Status::Ok;
    }
    auto const clear = !!(action & HarvestOptions::ResetUnaccessed);
    return m_page_table.harvest_accessed_pages(va, n, clear, visit, context, cleared);
}

auto ArchVmAspace::large_leaf_slot_empty(VirtAddr const va) -> bool {
    m_canary.verify<Self>();
    return m_page_table.second_level_empty(va);
}

auto ArchVmAspace::query(VirtAddr va, PhysAddr *pa, MmuFlags *mmuf, usize *size) -> Status {
    m_canary.verify<Self>();
    return m_page_table.query_mapping(va, pa, mmuf, size);
}

auto ArchVmAspace::active_cpus() const -> CpuMask {
    return m_active_cpus.load(std::memory_order_relaxed);
}

auto ArchVmAspace::open_ioports(u32 const base, u32 const count) -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(m_kind == VmasKind::User, "I/O ports are opened for a process's address space");
    return m_io_bitmap.open(base, count, m_active_cpus);
}

auto ArchVmAspace::close_ioports(u32 const base, u32 const count) -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(m_kind == VmasKind::User, "I/O ports are closed for a process's address space");
    return m_io_bitmap.close(base, count, m_active_cpus);
}

auto ArchVmAspace::protect(VirtAddr va, usize n, MmuFlags mmuf) -> Status {
    m_canary.verify<Self>();
    auto const status = m_page_table.protect_pages(va, n, mmuf);
    if (status == Status::Ok) {
        invalidate(va, n);
    }
    return status;
}

/// The TLB shootdown: after an entry has lost a permission or its page, every
/// CPU that may still hold the old translation drops it, and this returns
/// only once they all have.
///
/// Without it a CPU keeps using what its TLB holds. A clone takes Write off an
/// entry so that the next write faults and copies the page; another CPU that
/// cached the entry while it was writable writes straight through, into the
/// page the clone now shares. An unmap that frees a frame leaves another CPU
/// reading and writing the frame's next owner.
///
/// Who is asked depends on whose tables these are.
///
///  - A user address space: the CPUs in |m_active_cpus|, read after the
///    entries changed, behind a full barrier. switch_context() sets a CPU's
///    bit before that CPU loads these tables, so one that is not in the mask
///    either was not running here, and holds nothing (no PCIDs: loading
///    another root flushed it), or loads the tables after the change and
///    caches the new entry.
///  - The kernel's: every online CPU. Its leaf entries are Global
///    (X86Pte::make()), so loading another root does not drop them, and its
///    half is one top-level slot shared by every address space (alias_to()),
///    so a CPU running any process walks the kernel's tables and caches from
///    them. Its |m_active_cpus| names only the CPUs running kernel threads,
///    which says nothing about who holds a kernel translation.
///
///    What goes wrong without it is the reuse of an address: CPU1 caches
///    kernel address X to frame F; CPU0 unmaps X and frees F; the allocator,
///    lowest address first, hands X out again on a new frame; CPU1 goes on
///    reading and writing F, which now belongs to someone else.
///
///    While only this CPU is online -- the boot, before the APs start, when
///    the vmalloc self-test and reserve() unmap with interrupts off -- the
///    local invalidation is the whole shootdown, and sync_exec(), which
///    asserts interrupts are on, is not called.
///
/// When it is done, which the callers rely on:
///
///  - After the page table's spin lock is dropped, never under it. That lock
///    is held with interrupts off, and the other CPUs answer from their
///    interrupt handlers: one of them spinning for the same lock, interrupts
///    off, would never answer, and this one would never let go.
///    sync_exec() asserts interrupts are on.
///  - Before this returns, and so under the object's lock that the caller
///    (the clone, the fault path) holds. A CPU that still has the stale entry
///    can write through it until the shootdown completes; while the object's
///    lock is held nothing can copy that page in the meantime, so such a
///    write lands before any copy is taken, as if it had come before the
///    clone. Were the object's lock dropped first, a fault on another CPU could
///    copy the page, and the stale write would land in the original after the
///    copy, lost to the writer.
///
/// Each call is its own shootdown, the kernel's included; batching them is an
/// option recorded in docs/repairs/261002/x86-mm.md.
auto ArchVmAspace::invalidate(VirtAddr const va, usize const n) -> void {
    if (n == 0) {
        return;
    }

    struct Range {
        VirtAddr va;
        usize n;
    };
    auto range = Range{va, n};
    auto const invalidate_here = [](void *const context) {
        auto const &range = *static_cast<Range const *>(context);
        for (usize i = 0; i < range.n; ++i) {
            arch::x86_invlpg(range.va + i * PAGE_SIZE);
        }
    };

    CpuMask targets;
    if (m_kind == VmasKind::User) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        targets = m_active_cpus.load(std::memory_order_relaxed);
    } else {
        targets = Cpu::online_mask();
        if (targets.count() <= 1) {
            invalidate_here(&range);
            return;
        }
    }
    IpiService::sync_exec(targets, invalidate_here, &range);
}

/// invalidate() for every page at once: each CPU in |m_active_cpus| reloads
/// CR3, which drops every translation that is not Global. That is all of this
/// address space's on that CPU because its leaves are never Global (only the
/// kernel's are), and because no PCID is allocated: with one, a CPU that had
/// switched away would still hold this space's translations tagged with it,
/// which its reload of another root would not touch.
///
/// A CPU that left this address space after the mask was read reloads the
/// root it runs now, which costs a refill and nothing else. When it is done,
/// and why the barrier, is as for invalidate().
auto ArchVmAspace::invalidate_all() -> void {
    DEBUG_ASSERT(m_kind == VmasKind::User, "a whole-space flush of the kernel's tables");
    DEBUG_ASSERT(m_pcid == INVALID_PCID, "a whole-space flush by CR3 reload with a PCID");
    std::atomic_thread_fence(std::memory_order_seq_cst);
    auto const targets = m_active_cpus.load(std::memory_order_relaxed);
    IpiService::sync_exec(targets, [](void *) { arch::tlb_invalidate_all(); }, nullptr);
}
} // namespace ours::mm
