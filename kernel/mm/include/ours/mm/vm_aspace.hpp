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
    /// It used to return `Arc::adopt(KERNEL_ASPACE)`. In ustl that is not
    /// fbl's AdoptRef: `Rc(T *)` and `adopt()` retain (an object starts at a
    /// strong count of zero, and the first Rc makes it one), so each call was
    /// a balanced retain and release -- correct, and pure cost for a pointer
    /// that owns nothing.
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

    FORCE_INLINE
    auto get_lock() -> sync::Mutex & {
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

    /// Tears down everything mapped in this user address space; see the
    /// definition. Once only; the object itself lives on until its last
    /// reference goes.
    ///
    /// Zircon counterpart: VmAspace::Destroy().
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

    gktl::Canary m_canary{this};
    Kind m_kind;
    Flags m_flags;
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
    /// Zircon's counterpart is a CriticalMutex (vm_aspace.h), whose point is
    /// the timeslice extension that keeps a holder from being preempted in the
    /// middle of the critical section. sync::CriticalMutex exists here but is
    /// an empty shell -- it inherits Mutex protected and re-exposes nothing,
    /// so it cannot be acquired yet. A plain Mutex until it can.
    sync::Mutex m_lock;

    /// Set by destroy(), under m_lock. A destroyed aspace answers no more
    /// faults: its root VMA is gone.
    bool m_destroyed = false;

    /// For diagnostics. The constructor used to take a name and drop it.
    ktl::Name<32> m_name;

    /// Architecture specific context.
    ArchVmAspace m_arch;

    /// The only root region.
    ustl::Arc<VmArea> m_root_vma;
    ustl::Arc<VmMapping> m_last_fault;

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
