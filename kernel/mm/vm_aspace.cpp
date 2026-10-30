#include <ours/arch/aspace_layout.hpp>
#include <ours/assert.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>

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
/// destroyed.
///
/// Then the page tables, which destroy() had to leave: by now no CPU runs on
/// them (ArchVmAspace::destroy() gives the argument).
VmAspace::~VmAspace() {
    DEBUG_ASSERT(!m_root_vma && !m_last_fault,
                 "aspace '{}' destroyed without destroy() -- or a reference cycle broke", name());
    {
        Guard<sync::SpinLock, IrqSave> guard{ALL_ASPACE_LIST_LOCK};
        if (m_managed_hook.is_linked()) {
            auto _ = ALL_ASPACE_LIST.erase(ALL_ASPACE_LIST.iterator_to(*this));
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

    // Owned from here, so that a failed init() is undone by dropping this,
    // the only reference: ~VmAspace copes with what init() did not finish --
    // no root VMA, not yet on the list, page tables freed only if they were
    // made. It used to ignore init()'s answer and publish the half-built
    // address space anyway.
    auto owned = ustl::Arc<VmAspace>::adopt(aspace);
    if (auto const status = aspace->init(); status != Status::Ok) {
        return status;
    }
    {
        // Entrollment into the global list.
        Guard<sync::SpinLock, IrqSave> guard{ALL_ASPACE_LIST_LOCK};
        ALL_ASPACE_LIST.push_back(*aspace);
    }
    *out = ustl::move(owned);
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
    // it can be created with arbitrary permissions combination. It takes places its users choose
    // (VmaFlags::CanMapSpecific).
    constexpr auto ROOT_VMAF = VmaFlags::PermMask | VmaFlags::Share | VmaFlags::CanMapSpecific;

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
/// been switched away from for the last time. So the work is split: this
/// tears the regions down, ~VmAspace() frees the tables.
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
    ustl::Arc<VmMapping> vdso;
    Status status;
    {
        Guard guard{m_lock};
        if (!!(m_flags & Flags::Destroyed)) {
            return Status::BadState;
        }
        m_flags |= Flags::Destroyed;

        last_fault = ustl::move(m_last_fault);
        // The vDSO's mapping is one of the root's, gone with it below. Its
        // lock-free copy stays: no thread of a destroyed aspace asks it.
        vdso = ustl::move(m_vdso_mapping);
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

/// This and set_vdso() are out of line only because VmMapping is incomplete in
/// the header, and copying an Arc of it retains it.
auto VmAspace::vdso() const -> ustl::Arc<VmMapping> {
    Guard guard{m_lock};
    return m_vdso_mapping;
}

/// The Arc goes in and the range is published, base first and the size last
/// with release as vdso_contains() expects, all under m_lock.
/// withdraw_vdso_locked() writes the size as well, under the same lock, so
/// the lock decides which of the two writes last, rather than an argument
/// about which thread can reach the mapping at the time. Called once per
/// registration: the claim admits one caller, and the check below keeps a
/// second call from moving a range a reader may already trust.
auto VmAspace::set_vdso(ustl::Arc<VmMapping> mapping) -> void {
    DEBUG_ASSERT(mapping, "registered an empty mapping");
    auto const base = mapping->base();
    auto const size = mapping->size();
    {
        Guard guard{m_lock};
        DEBUG_ASSERT(!!(m_flags & Flags::Vdso), "registered without a claim");
        DEBUG_ASSERT(!m_vdso_mapping, "registered twice");
        // destroy() ran between the mapping and here; it took the mapping
        // down with the root. Keeping it would make a cycle nothing breaks: a
        // mapping holds an Arc to its aspace, and only destroy() resets this
        // one. So it is dropped instead, after the lock, and nothing is
        // published: no thread of a destroyed aspace makes a system call.
        if (!!(m_flags & Flags::Destroyed)) {
            return;
        }
        m_vdso_mapping = ustl::move(mapping);
        m_vdso_code_base.store(base, std::memory_order_relaxed);
        m_vdso_code_size.store(size, std::memory_order_release);
    }
}

/// The size alone goes to zero, and the base stays. vdso_contains() loads
/// the size first, so a reader that loaded the old size just before this
/// store then loads a base, and that must be one written together with that
/// size: this registration's, or a later one's, where the vDSO's code lies
/// again with the same size, since there is one vDSO. Zeroing the base too
/// would let that reader pair the old size with base 0 and accept a system
/// call from [0, size).
///
/// Relaxed, because nothing is published with it and it races no writer:
/// set_vdso() writes under the same lock, and nothing new can be mapped where
/// the code was until this lock is released, whose release orders the store
/// before that. The reference let go of here is not the mapping's last:
/// VmMapping::destroy(), the one caller, runs while its parent's set still
/// holds one.
auto VmAspace::withdraw_vdso_locked() -> void {
    m_vdso_code_size.store(0, std::memory_order_relaxed);
    m_flags &= ~Flags::Vdso;
    m_vdso_mapping = {};
}

/// Because of cyclic dependency between VmAspace and VmArea, there is not yet
/// a better way to inline it.
auto VmAspace::root_vma() -> ustl::Arc<VmArea> {
    return m_root_vma;
}

/// The root is read under the lock, after the Destroyed check, as the fault
/// path reads it: destroy() moves it out under the same lock, so a read
/// without it, as root_vma() does, could find it gone mid-walk.
auto VmAspace::for_each_node(void *const context, NodeVisitor const visit) -> Status {
    m_canary.verify<Self>();

    Guard<sync::Mutex> guard{m_lock};
    if (!!(m_flags & Flags::Destroyed)) {
        return Status::BadState;
    }
    // The root is at depth 1 here, one more than descend() counts it.
    m_root_vma->descend([context, visit](VmAreaOrMapping &node, usize const depth) {
        visit(context, node, depth + 1);
    });
    return Status::Ok;
}

auto VmAspace::harvest_accessed(AccessedVisitor const visit, void *const context) -> usize {
    m_canary.verify<Self>();

    Guard<sync::Mutex> guard{m_lock};
    if (!!(m_flags & Flags::Destroyed)) {
        return 0;
    }
    constexpr auto ACTION = HarvestOptions::UpdateAge | HarvestOptions::ResetUnaccessed;
    usize cleared = 0;
    m_root_vma->descend([&](VmAreaOrMapping &node, usize) {
        if (!node.is_mapping()) {
            return;
        }
        auto &mapping = static_cast<VmMapping &>(node);
        auto vmo = mapping.vmo();
        if (as<VmObjectPaged>(vmo.as_ptr_mut()) == nullptr) {
            return;
        }
        usize here = 0;
        auto const status = m_arch.harvest_accessed(mapping.base(), mapping.size() / PAGE_SIZE,
                                                    ACTION, visit, context, &here);
        // A mapping's leaves lie inside it, so no walk can meet a large leaf
        // it covers in part, the one thing that fails.
        DEBUG_ASSERT(Status::Ok == status, "harvesting a mapping of '{}' failed: {}", name(),
                     to_string(status));
        cleared += here;
    });
    if (cleared != 0) {
        m_arch.invalidate_all();
    }
    return cleared;
}

/// One space at a time, and without ALL_ASPACE_LIST_LOCK while it is
/// harvested: that lock is a spin lock, and a harvest takes the space's mutex.
/// So the walk holds a reference to the space it is on, which keeps it on the
/// list (its destructor is what takes it off), steps from it to the next under
/// the lock, and lets go of it after the lock.
///
/// The next one is taken with try_adopt(): a space on the list may already
/// have lost its last reference and be waiting in its destructor for the lock
/// this holds, and a reference cannot be taken to it any more; it is passed
/// over. Letting go of the previous one after the lock is not a nicety either:
/// that may be its last reference, and its destructor takes the lock.
auto VmAspace::harvest_all_user_accessed(AccessedVisitor const visit, void *const context)
    -> usize {
    usize cleared = 0;
    ustl::Arc<Self> current;
    while (true) {
        ustl::Arc<Self> next;
        {
            Guard<sync::SpinLock, IrqSave> guard{ALL_ASPACE_LIST_LOCK};
            auto at = ALL_ASPACE_LIST.begin();
            if (current) {
                at = ALL_ASPACE_LIST.iterator_to(*current);
                ++at;
            }
            for (; at != ALL_ASPACE_LIST.end(); ++at) {
                if (!at->is_user()) {
                    continue;
                }
                next = ustl::Arc<Self>::try_adopt(&*at);
                if (next) {
                    break;
                }
            }
        }
        current = ustl::move(next);
        if (!current) {
            return cleared;
        }
        cleared += current->harvest_accessed(visit, context);
    }
}

auto VmAspace::find_mapping(VirtAddr const va, ustl::Arc<VmMapping> *const out) -> Status {
    m_canary.verify<Self>();

    Guard<sync::Mutex> guard{m_lock};
    if (!!(m_flags & Flags::Destroyed)) {
        return Status::BadState;
    }
    m_root_vma->get_lock().assert_held();
    auto *const mapping = m_root_vma->find_mapping_locked(va);
    if (mapping == nullptr) {
        return Status::NotFound;
    }
    // Alive under the lock: the tree holds a reference to every node in it,
    // as the fault path relies on when it caches one (page_fault_internal()).
    *out = ustl::Arc<VmMapping>::adopt(mapping);
    return Status::Ok;
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
            // reached in practice.
            if (!!(m_flags & Flags::Destroyed)) {
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

        // The page is not there yet: a VMO with a page source raised a request
        // for it, and the provider has to supply it. Waited on here, with no
        // lock held, since the provider needs the VMO's lock to supply and may
        // need this address space's to get the content; then the fault is
        // taken again from the top, because anything may have changed while
        // this thread slept.
        if (Status::ShouldWait == status) {
            auto const waited_status = vmf.page_request.wait(!!(cause & VmfCause::User));
            if (Status::Ok != waited_status) {
                // A provider that never answered: show what this address
                // space maps. Under the lock, which the wait had to be without:
                // the tree of areas is walked, and another thread may be
                // changing it. And only while the address space still has its
                // root, which destroy() takes away -- a wait can outlast the
                // process that started it.
                if (Status::TimeOut == waited_status) {
                    Guard guard{get_lock()};
                    if (!(m_flags & Flags::Destroyed)) {
                        m_root_vma->get_lock().assert_held();
                        m_root_vma->dump_locked();
                    }
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