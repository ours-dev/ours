#include <ours/arch/aspace_layout.hpp>
#include <ours/assert.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>

#include <arch/interrupt.hpp>
#include <gktl/counter.hpp>
#include <gktl/init_hook.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/lazy_init.hpp>

namespace {
GKTL_COUNTER(VM_ASPACE_HIGH_PRIORITY, "vm.aspace.high_priority")
GKTL_COUNTER(VM_ASPACE_ACCESSED_HARVESTS_PERFORMED, "vm.aspace.accessed_harvest.performed")
GKTL_COUNTER(VM_ASPACE_ACCESSED_HARVESTS_SKIPPED, "vm.aspace.accessed_harvest.skipped")
GKTL_COUNTER(VM_ASPACE_LAST_FAULT_HIT, "vm.aspace.last_fault.hit");
GKTL_COUNTER(VM_ASPACE_LAST_FAULT_MISS, "vm.aspace.last_fault.miss");
} // namespace

namespace ours::mm {
using AspaceCache = mm::ObjectCache::Instance<VmAspace>;

decltype(VmAspace::ALL_ASPACE_LIST) VmAspace::ALL_ASPACE_LIST;
decltype(VmAspace::ALL_ASPACE_LIST_LOCK) VmAspace::ALL_ASPACE_LIST_LOCK;

decltype(VmAspace::KERNEL_ASPACE) VmAspace::KERNEL_ASPACE;
decltype(VmAspace::KERNEL_ASPACE_LOCK) VmAspace::KERNEL_ASPACE_LOCK;

// Initializer order follows the member declaration order in vm_aspace.hpp.
// m_refcnt is owned by the ustl::RefCounted base (initialized by Base()), and
// m_num_users belongs to VmPage, not here -- both were leftovers.
VmAspace::VmAspace(VirtAddr base, usize size, Kind kind, VmasFlags flags, char const *name)
    : Base()
    , m_kind(kind)
    , m_flags(flags)
    , m_base(base)
    , m_size(size)
    , m_name(name)
    , m_arch(base, size, kind)
    , m_root_vma() {}

/// The last reference is gone, which for a user aspace can only happen after
/// destroy(): until then the root VMA and every mapping under it hold one.
///
/// Out of ALL_ASPACE_LIST first, under its lock. The hook is a SafeLink, whose
/// destructor traps on a node still linked -- and nothing took an aspace off
/// the list before, because until destroy() existed no aspace was ever
/// destroyed. Zircon's ~VmAspace erases from its list at the same point.
///
/// Then the page tables, which destroy() had to leave: by now no CPU runs on
/// them (ArchVmAspace::destroy() gives the argument). Zircon's ~VmAspace calls
/// arch_aspace_.Destroy() at the same point, after the same erase.
VmAspace::~VmAspace() {
    DEBUG_ASSERT(!m_root_vma && !m_last_fault,
                 "aspace '{}' destroyed without destroy() -- or a reference cycle broke", name());
    {
        Guard<sync::SpinLock, IrqSave> guard{ALL_ASPACE_LIST_LOCK};
        if (m_managed_hook.is_linked()) {
            static_cast<void>(ALL_ASPACE_LIST.erase(ALL_ASPACE_LIST.iterator_to(*this)));
        }
    }

    auto const teardown = m_arch.destroy();
    if (teardown.leaves != 0) {
        log::warn("mm: aspace '{}' still had {} pages mapped when its tables went", name(),
                  teardown.leaves);
    }
    // With the free-frame count in a debug build: taken after every page this
    // process held is back, the difference between two of these lines is
    // exactly what the process leaked, or what is still in flight.
    if constexpr (KERNEL_DEBUG_LEVEL > 0) {
        log::debug("mm: aspace '{}' gone, {} page-table pages freed, {} frames free", name(),
                   teardown.tables, free_frame_count());
    }
}

/// The range an address space of |kind| covers.
///
/// A switch and not an if/else chain: the kernel range used to be the `else`,
/// so a Guest aspace -- and, once the User test turned out to be broken, a
/// user aspace too -- silently got the kernel's base and size. With every
/// kind named, adding a fourth is a compile error here rather than a fourth
/// thing that quietly becomes the kernel.
auto VmAspace::create(Kind kind, VmasFlags flags, char const *name, ustl::Arc<VmAspace> *out)
    -> Status {
    VirtAddr base;
    usize size;
    switch (kind) {
        case Kind::User:
            base = USER_ASPACE_BASE;
            size = USER_ASPACE_SIZE;
            break;
        case Kind::Kernel:
            base = KERNEL_ASPACE_BASE;
            size = KERNEL_ASPACE_SIZE;
            break;
        case Kind::Guest:
            // A guest's range is the guest's business, not this kernel's:
            // it comes from the hypervisor with the EPT it is paired with.
            // Refusing is better than lending it the kernel's numbers.
            return Status::Unimplemented;
    }

    return Self::create(base, size, kind, flags, name, out);
}

auto VmAspace::create(VirtAddr base, usize size, Kind kind, Flags flags, char const *name,
                      ustl::Arc<VmAspace> *out) -> Status {
    auto const aspace = AspaceCache::get(GAF_KERNEL, base, size, kind, flags, name);
    if (!aspace) {
        return Status::OutOfMem;
    }

    aspace->init();
    {
        // Entrollment into the global list.
        Guard<sync::SpinLock, IrqSave> guard{ALL_ASPACE_LIST_LOCK};
        ALL_ASPACE_LIST.push_back(*aspace);
    }
    *out = ustl::Arc<VmAspace>::adopt(aspace);
    return Status::Ok;
}

INIT_CODE
auto VmAspace::init_kernel_aspace() -> void {
    constexpr auto FLAGS = VmasFlags::None;

    auto status = AspaceCache::init("aspace-cache", ObjectCache::Flags::Folio);
    if (Status::Ok != status) {
        panic("Failed to create object cache for VmAspace");
    }

    ustl::Arc<Self> aspace;
    status = Self::create(KERNEL_ASPACE_BASE, KERNEL_ASPACE_SIZE, Kind::Kernel, FLAGS, "K:Aspace",
                          &aspace);
    ASSERT(Status::Ok == status, "Failed to create kernel VmAspace");

    Self::KERNEL_ASPACE = aspace.into_raw();
}

auto VmAspace::sync_kernel_aspace() -> void {}

/// Either side may be null, and null means "the kernel address space".
///
/// A kernel thread carries no aspace of its own -- Thread::m_aspace stays
/// null for one -- so the scheduler reaches here with a null `prev` the first
/// time it switches to a user thread, and a null `next` when it switches back.
/// Taking `&prev->m_arch` on a null pointer is undefined even though nothing
/// dereferences it afterwards (ArchVmAspace::switch_context ignores `from`
/// entirely), and it only happened to work because m_arch is not at offset 0.
auto VmAspace::switch_aspace(Self *prev, Self *next) -> void {
    ArchVmAspace::switch_context(prev ? &prev->m_arch : nullptr, next ? &next->m_arch : nullptr);
}

auto VmAspace::clone(VmasFlags flags, ustl::Arc<VmAspace> *out) -> Status {
    return Status::Unimplemented;
}

auto VmAspace::init() -> Status {
    auto status = m_arch.init();
    if (status != Status::Ok) {
        return status;
    }

    // In the normal case, the root VMA always ownes RWX permissions so that the sub-VMA derived from
    // it can be created with arbitrary permissions combination.
    constexpr auto ROOT_VMAF = VmaFlags::PermMask | VmaFlags::Share;

    status = VmArea::create_root(m_base, m_size, ROOT_VMAF, this, "Root", &m_root_vma);
    if (status != Status::Ok) {
        return Status::OutOfMem;
    }

    return Status::Ok;
}

/// Unmaps everything in this user address space and lets go of the tree.
///
/// Called as the process that owns it becomes Dead, from
/// ProcessDispatcher::finish_dead_transition(): usually on the process's last
/// thread, whose CR3 still points at these page tables. So nothing is freed
/// that the CPU may still walk -- the mappings go, and with them the VMOs only
/// they referenced (a program's image, its stack), but the page-table pages
/// stay until the object itself is destroyed, by which time that thread has
/// been switched away from for the last time. Zircon splits it the same way:
/// VmAspace::Destroy() tears the regions down, ~VmAspace frees the tables
/// (the comment in its vm_aspace.cc explains the running thread).
///
/// Before this, nothing ever destroyed an address space, and nothing could:
/// the root VMA holds an Arc to its aspace and the aspace holds the root VMA
/// (every mapping holds one too), and m_last_fault kept a mapping alive. So a
/// dead process's memory -- all of it -- was never returned. Moving the root
/// VMA and the last-fault cache out is what breaks those cycles; they are
/// released after the lock, because releasing them can run ~VmMapping and
/// free a VMO's pages.
///
/// The VMAs keep their own Arc<VmAspace>: a VMA handle can outlive the
/// process, and a region takes its lock through that pointer. Such a handle
/// finds its region inactive and every operation answers BadState.
auto VmAspace::destroy() -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(is_user(), "only a user address space is ever destroyed");

    ustl::Arc<VmArea> root;
    ustl::Arc<VmMapping> last_fault;
    Status status;
    {
        Guard guard{m_lock};
        if (m_destroyed) {
            return Status::BadState;
        }
        m_destroyed = true;

        last_fault = ustl::move(m_last_fault);
        // m_lock is the root VMA's lock; the analysis only sees two names.
        m_root_vma->get_lock().assert_held();
        status = m_root_vma->destroy();
        root = ustl::move(m_root_vma);
    }

    if (Status::Ok != status) {
        log::warn("mm: tearing down aspace '{}' left something mapped: {}", name(),
                  to_string(status));
    }
    log::debug("mm: aspace '{}' destroyed", name());
    return status;
}

/// Because of cyclic dependency between VmAspace and VmArea, there is not yet
/// a better way to inline it.
auto VmAspace::root_vma() -> ustl::Arc<VmArea> {
    return m_root_vma;
}

auto VmAspace::page_fault_internal(VirtAddr va, VmfCause cause, usize extra_pages) -> Status {
    m_canary.verify<Self>();

    // Every caller must be able to block, and this is where that is enforced
    // for both entries: page_fault() from the architecture's fault handler and
    // soft_fault() from kernel code that has just dropped its locks to resolve
    // a fault out of line.
    //
    // Resolving may block: m_lock is a mutex, the VMO's lock beneath it will be
    // one, and supplying a page can wait on reclaim. So this is not a question
    // this function can answer by refusing -- there is no fault it could
    // resolve without blocking -- only one it can check its callers got right.
    // This used to be a VmfCause::MaySleep bit the fault entry set and this
    // function refused without. The hardware path now stops the kernel before
    // any fault that may not sleep reaches here (x86_handle_page_fault()), which
    // left the bit redundant there and wrong everywhere else: soft_fault() never
    // set it, so every soft fault was refused.
    //
    // Zircon asks the same of its callers in the same place --
    // PageFaultInternal() opens with lockdep::AssertNoLocksHeld() -- and
    // SoftFault() carries the same contract in its header: may block, must be
    // called without locks held.
    //
    // Two of the four atomic conditions are checked. Interrupt-handler context
    // is implemented here as raised preemption, and preemption itself cannot be
    // checked against a fixed value in this shared path: a hardware fault
    // arrives with the exception preamble's count of one, a soft fault with
    // none. The fault entry checks preemption against its own baseline; a soft
    // fault caller in an interrupt handler would already have a spin lock or
    // masked interrupts to be caught by below, as every spin lock in this tree
    // is held with interrupts off.
    //
    // ASSERT rather than DEBUG_ASSERT for the reason the fault entry stops the
    // kernel in release builds as well: with the check compiled out, a violation
    // does not fail, it hangs on a mutex that can never be released to it.
    ASSERT(sync::SpinLock::num_held_cpu() == 0,
           "page fault at {:#x} resolved with {} spin lock(s) held on this CPU", va,
           sync::SpinLock::num_held_cpu());
    ASSERT(!arch::irqs_disabled(), "page fault at {:#x} resolved with interrupts disabled", va);

    auto status = Status::Ok;
    // The fault descriptor lives here, outside the loop, so that its
    // MultiPageRequest survives every retry: a request raised on one pass has
    // to be the same object waited on and completed on the next. It is on this
    // thread's stack because the fault path must not allocate -- an allocation
    // can recurse into reclaim and fault again.
    VmFault vmf{.va = va, .num_pages = extra_pages + 1, .cause = cause, .page_request = {}};
    do {
        u32 num_mapped = 0;
        {
            // Grabs the aspace's lock to prevent another operation on it from users.
            Guard guard{get_lock()};

            // Nothing is mapped in a destroyed aspace, and its root VMA is gone.
            // No thread of a dead process runs user code, so this is not
            // reached in practice; Zircon asserts it (VmAspace::PageFault()).
            if (m_destroyed) {
                return Status::NotFound;
            }

            // m_last_fault is an empty Arc until the first lookup succeeds, so
            // the cache test has to ask whether there is a mapping at all
            // before asking whether it covers |va|. Without the first half
            // this read check_range()'s members off a null `this`, on the very
            // first page fault the kernel ever took -- which is to say the
            // cache was unusable exactly once, at the only moment it could not
            // be recovered from.
            if (m_last_fault && m_last_fault->check_range(vmf.va, 1)) [[likely]] {
                VM_ASPACE_LAST_FAULT_HIT.add(1);
            } else {
                VM_ASPACE_LAST_FAULT_MISS.add(1);
                m_root_vma->get_lock().assert_held();
                m_last_fault = ustl::Arc<VmMapping>::adopt(m_root_vma->find_mapping_locked(vmf.va));
                if (!m_last_fault) {
                    return Status::NotFound;
                }
            }

            m_last_fault->get_lock().assert_held();
            auto const [final_status, mapped] = m_last_fault->page_fault(&vmf);
            status = final_status;
            num_mapped = static_cast<u32>(mapped);
        }

        // There is no sufficient pages to map, so we need to wait for the page
        // request to be fulfilled.
        //
        // Unreachable today and kept deliberately: no VMO carries a page
        // source, so an anonymous miss is answered by allocating on the spot
        // and never returns ShouldWait. The shape stays so that adding the
        // request protocol later is filling this in rather than rewriting the
        // entry point around it. See docs/repairs/260903/mm.md item 3.
        if (Status::ShouldWait == status) {
            auto const waited_status = vmf.page_request.wait(!!(cause & VmfCause::User));
            if (Status::Ok != waited_status) {
                if (Status::TimeOut == waited_status) {
                    m_root_vma->dump_locked();
                }
                return waited_status;
            }

            if (num_mapped > 0) {
                vmf.va += num_mapped * PAGE_SIZE;

                DEBUG_ASSERT(vmf.num_pages > num_mapped);
                vmf.num_pages -= num_mapped;
            }
        }
    } while (Status::ShouldWait == status);

    return status;
}

auto VmAspace::page_fault(VirtAddr va, VmfCause cause) -> Status {
    return page_fault_internal(va, cause, 0);
}

auto VmAspace::soft_fault(VirtAddr va, VmfCause cause) -> Status {
    return page_fault_internal(va, cause | VmfCause::Software, 0);
}
} // namespace ours::mm