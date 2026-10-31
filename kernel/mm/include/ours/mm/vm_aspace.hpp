/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_ASPACE_HPP
#define OURS_MM_VM_ASPACE_HPP 1

#if !__has_include(<ours/mm/arch_vm_aspace.hpp>)
#error "The header <ours/mm/arch_vm_aspace.hpp> is required by [protocol.module.mem]"
#endif
#include <ours/init.hpp>
#include <ours/mm/arch_vm_aspace.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/mm/vmas_flags.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/sync/spin_lock.hpp>

#include <atomic>

#include <gktl/canary.hpp>
#include <gktl/range.hpp>
#include <ktl/name.hpp>
#include <ktl/result.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/rc.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::mm {
class VmAspace: public ustl::RefCounted<VmAspace, ustl::sync::ThreadSafePolicy> {
    using Self = VmAspace;
    using Base = ustl::RefCounted<VmAspace, ustl::sync::ThreadSafePolicy>;

  public:
    using Kind = VmasKind;
    using Flags = VmasFlags;

    struct Usage {
        usize mapped_pages;
        usize private_pages;
        usize shared_pages;
    };

    static auto clone(Flags, ustl::Arc<VmAspace> *) -> Status;

    static auto create(Kind, Flags, char const *, ustl::Arc<VmAspace> *out) -> Status;

    static auto create(VirtAddr, usize, Kind, Flags, char const *, ustl::Arc<VmAspace> *) -> Status;

    /// Perform the initialization of kernel address space.
    /// Just once called at boot-time.
    static auto init_kernel_aspace() -> void;

    /// Synchronize changes from kernel's address space into all user's.
    static auto sync_kernel_aspace() -> void;

    static auto switch_aspace(Self *, Self *) -> void;

    /// The kernel address space, which nobody owns.
    ///
    /// A bare pointer rather than an Arc, because there is no ownership here
    /// to express: the kernel aspace is a singleton built once by
    /// init_kernel_aspace() and never destroyed, and every caller in the tree
    /// uses it as `kernel_aspace()->something` and drops it immediately.
    ///
    /// It used to return `Arc::adopt(KERNEL_ASPACE)`. In ustl that is not an
    /// adopt-without-retain: `Rc(T *)` and `adopt()` retain (an object starts
    /// at a strong count of zero, and the first Rc makes it one), so each call
    /// was a balanced retain and release -- correct, and pure cost for a
    /// pointer that owns nothing.
    ///
    /// Null until init_vmm() has run. Callers that run earlier -- the boot
    /// thread is built in task::init_early(), before any address space exists
    /// -- have to be attached later rather than ask now.
    static auto kernel_aspace() -> VmAspace * {
        return KERNEL_ASPACE;
    }

    auto get_memory_usage() -> ktl::Result<Usage>;

    auto page_fault(VirtAddr va, VmfCause cause) -> Status;

    auto soft_fault(VirtAddr va, VmfCause cause) -> Status;

    auto accessed_fault(VirtAddr va) -> Status;

    TA_RETURN_CAPABILITY(m_lock) FORCE_INLINE auto get_lock() -> sync::Mutex & {
        return m_lock;
    }

    FORCE_INLINE
    auto is_user() const -> bool {
        return m_kind == Kind::User;
    }

    FORCE_INLINE
    auto arch_aspace() -> ArchVmAspace & {
        return m_arch;
    }

    auto root_vma() -> ustl::Arc<VmArea>;

    /// Where the space starts and how large it is; fixed when it is built.
    [[nodiscard]]
    FORCE_INLINE
    auto base() const -> VirtAddr {
        return m_base;
    }

    [[nodiscard]]
    FORCE_INLINE
    auto size() const -> usize {
        return m_size;
    }

    /// What for_each_node() calls for each node, with the context it was
    /// given, under the space's lock.
    using NodeVisitor = void (*)(void *context, VmAreaOrMapping &node, usize depth);

    /// Calls |visit| for the root area, at depth 1, and for every area and
    /// mapping under it, depth first in address order (VmArea::
    /// VmAreaOrMapping::descend()), all under the space's lock: the shape of a dump of
    /// the space. |visit| runs with that lock held, so it must not copy to or
    /// from user memory, allocate, or take any lock but an object's, which
    /// nests inside it; it reads nodes, nothing else. BadState once the space
    /// is destroyed: its root area is gone.
    auto for_each_node(void *context, NodeVisitor visit) -> Status;

    /// Asks |visit|(context, pa, size) about every leaf with its accessed bit
    /// set in this space's mappings of paged objects, and clears the bits it
    /// answers true for; then, if any was cleared, drops this space's
    /// translations from every CPU running it, in one shootdown, so that the
    /// next access walks the table and sets the bit again. How many bits it
    /// cleared; 0 once the space is destroyed.
    ///
    /// Only paged objects: every frame such a mapping maps is a page of its
    /// object, or the shared zero page. A physical object's frames are not
    /// pages any page queue knows. Under the space's lock, so the mappings
    /// stay; a page whose object unmaps it meanwhile is safe as well, since
    /// the visitor runs under the page table's lock, which that unmap needs
    /// before the page can be freed.
    auto harvest_accessed(AccessedVisitor visit, void *context) -> usize;

    /// harvest_accessed() on every user address space there is. How many bits
    /// it cleared in all.
    static auto harvest_all_user_accessed(AccessedVisitor visit, void *context) -> usize;

    /// The mapping that holds |va|, into |out|, with a reference of its own,
    /// so that it and its object outlive the lock this takes and drops.
    /// NotFound if no mapping holds it, BadState once the space is destroyed.
    auto find_mapping(VirtAddr va, ustl::Arc<VmMapping> *out) -> Status;

    /// The vDSO of this address space: the mapping of its code, which is the
    /// only place the kernel accepts a `syscall` from (vdso_contains()).
    ///
    /// Registered once, from the mapping of the vDSO's VMO and by nothing else
    /// (object::VmaDispatcher::map()), in three steps, because the mapping
    /// itself takes m_lock (it is the root VMA's lock) and so cannot run under
    /// it:
    ///   1. claim_vdso() sets Flags::Vdso under m_lock, so that two mappings
    ///      racing for the vDSO cannot both win;
    ///   2. the caller maps it, without the lock;
    ///   3. set_vdso() stores the mapping, or release_vdso() clears the bit if
    ///      the mapping failed, so that a failed call does not keep the vDSO
    ///      from ever being mapped.
    ///
    /// Withdrawn only with its mapping, and only while nobody knows where it
    /// is: the call that registered it destroys the mapping again when it
    /// cannot report the address (VmArea::destroy_child()), and
    /// VmMapping::destroy() withdraws the registration on the way
    /// (withdraw_vdso_locked()). The range is then accepted no more, and the
    /// claim is free for the next mapping of the vDSO.
    ///
    /// Two copies of where it is, for two kinds of reader:
    ///   - m_vdso_mapping, the mapping itself, under m_lock: for whoever
    ///     manages it, through vdso();
    ///   - m_vdso_code_base and m_vdso_code_size, written under m_lock by
    ///     set_vdso() and withdraw_vdso_locked() and read without a lock on
    ///     every system call, through vdso_contains(). A mutex there would
    ///     put every system call of the process behind every map and fault on
    ///     its address space.
    [[nodiscard]]
    auto vdso() const -> ustl::Arc<VmMapping>;

    /// Step 1 of the registration, see vdso(). False if another mapping holds
    /// the claim or has registered already.
    [[nodiscard]]
    auto claim_vdso() -> bool {
        Guard<sync::Mutex> guard{m_lock};
        if (!!(m_flags & Flags::Vdso)) {
            return false;
        }
        m_flags |= Flags::Vdso;
        return true;
    }

    /// Step 3 of a registration whose mapping failed: gives the claim back.
    auto release_vdso() -> void {
        Guard<sync::Mutex> guard{m_lock};
        DEBUG_ASSERT(!m_vdso_mapping, "released after it was registered");
        m_flags &= ~Flags::Vdso;
    }

    /// Step 3 of a registration whose mapping succeeded: records it, and
    /// publishes where it is for vdso_contains(). Unless the aspace was
    /// destroyed in between: then the mapping is dropped; see the definition.
    auto set_vdso(ustl::Arc<VmMapping> mapping) -> void;

    /// Whether the vDSO is registered. Read from the lock-free copy, so that
    /// it agrees with vdso_contains().
    [[nodiscard]]
    FORCE_INLINE
    auto has_vdso() const -> bool {
        return m_vdso_code_size.load(std::memory_order_acquire) != 0;
    }

    /// Whether all of [addr, addr + len) lies inside the vDSO's code.
    ///
    /// A range and not a point, because what is asked about is an instruction
    /// -- the `syscall` a trap came from -- and all of its bytes have to be the
    /// vDSO's.
    ///
    /// Without a lock. set_vdso() stores the base first and the size last,
    /// with release; this loads the size first, with acquire. A reader that
    /// sees a size therefore sees the base that goes with it, and one that
    /// sees none answers false for every range: a process without a vDSO can
    /// make no system call at all rather than any. A withdrawal zeroes the
    /// size alone and leaves the base, so a reader that loaded the size just
    /// before it still pairs it with a base some registration wrote; see
    /// withdraw_vdso_locked().
    ///
    /// Written as two comparisons that cannot wrap: |len| no larger than the
    /// vDSO, then |addr|'s offset into it no further than what leaves room for
    /// |len|. The subtraction wraps an |addr| below the base to a huge offset,
    /// which the second comparison refuses -- the same shape
    /// ours::mm::is_kernel_address() uses for one end.
    [[nodiscard]]
    FORCE_INLINE
    auto vdso_contains(VirtAddr addr, usize len) const -> bool {
        auto const size = m_vdso_code_size.load(std::memory_order_acquire);
        return len <= size && addr - m_vdso_code_base.load(std::memory_order_relaxed) <= size - len;
    }

    /// Tears down everything mapped in this user address space; see the
    /// definition. Once only; the object itself lives on until its last
    /// reference goes.
    auto destroy() -> Status;

    [[nodiscard]]
    FORCE_INLINE
    auto name() const -> char const * {
        return m_name.data();
    }

    VmAspace(VirtAddr, usize, Kind, Flags, char const *);
    ~VmAspace();

  private:
    friend class VmArea;
    friend class VmMapping;

    struct AlsrConfig {
        u8 entropy_bits;
        u8 compact_entropy_bits;
        // We record the PRNG seed to enable reproducible debugging.
        u8 seed[2];
    };

    auto init() -> Status;

    auto page_fault_internal(VirtAddr va, VmfCause cause, usize extra_pages) -> Status;

    /// Undoes set_vdso() and claim_vdso() together, for VmMapping::destroy()
    /// when the mapping going is the registered one: no range is accepted
    /// any more, the claim is free again, and the mapping is let go of. See
    /// vdso().
    TA_REQUIRES(m_lock)
    auto withdraw_vdso_locked() -> void;

    gktl::Canary m_canary{this};
    Kind m_kind;
    /// Destroyed and Vdso are written after construction, both under m_lock;
    /// every bit of the word has to be, or two read-modify-writes of it
    /// racing would erase each other's bit.
    Flags m_flags TA_GUARDED(m_lock);
    VirtAddr m_base;
    VirtAddr m_size;
    /// Sleepable, and it has to be.
    ///
    /// It was a spin lock, held with interrupts off, because the #PF handler
    /// took it with interrupts still off. That forced every other holder --
    /// map, protect, unmap, destroy -- to match, and worse, it made this lock
    /// unable to nest a mutex inside it. The VMO's page tree is guarded by
    /// the object's own lock, VmObject::lock(), so the fault path could not take that lock and
    /// went around it, leaving VmCowPages::m_pages written under two unrelated
    /// locks. That is item 1 of docs/repairs/260925/mm.md, and this is what
    /// unblocks the fix: the fault path now takes this lock and then the VMO's,
    /// in that order.
    ///
    /// Reaching a sleepable context took the #PF handler enabling interrupts
    /// first, which it does only where sleeping is allowed; a fault from a
    /// context that may not sleep stops the kernel there instead. Every path
    /// into page_fault_internal() -- that handler and soft_fault() -- must
    /// therefore arrive able to block, and page_fault_internal() asserts it.
    ///
    /// The lock this should be is a sync::CriticalMutex, whose point is the
    /// timeslice extension that keeps a holder from being preempted in the
    /// middle of the critical section. sync::CriticalMutex exists but is an
    /// empty shell -- it inherits Mutex protected and re-exposes nothing,
    /// so it cannot be acquired yet. A plain Mutex until it can.
    mutable sync::Mutex m_lock;

    /// For diagnostics. The constructor used to take a name and drop it.
    ktl::Name<32> m_name;

    /// Architecture specific context.
    ArchVmAspace m_arch;

    /// See vdso(). Empty until registered, and again once destroyed.
    ustl::Arc<VmMapping> m_vdso_mapping TA_GUARDED(m_lock);

    /// See vdso_contains(). Zero size until registered, and again once
    /// withdrawn, which is what makes vdso_contains() refuse everything. The
    /// address space's teardown leaves them as they are: no thread of a
    /// destroyed address space runs user code to ask.
    std::atomic<VirtAddr> m_vdso_code_base{0};
    std::atomic<usize> m_vdso_code_size{0};

    /// The only root region.
    ustl::Arc<VmArea> m_root_vma;
    ustl::Arc<VmMapping> m_last_fault TA_GUARDED(m_lock);

    AlsrConfig m_alsr_config;

    ustl::collections::intrusive::ListMemberHook<> m_managed_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOptions);
    USTL_DECLARE_LIST(Self, List, ManagedOptions);

    /// The list which strungs all existing VmAspace.
    static Self::List ALL_ASPACE_LIST;
    static sync::SpinLock ALL_ASPACE_LIST_LOCK;

    static VmAspace *KERNEL_ASPACE;
    static sync::SpinLock KERNEL_ASPACE_LOCK;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_ASPACE_HPP
