#include <gktl/counter.hpp>
#include <gktl/init_hook.hpp>
#include <ktl/new.hpp>
#include <logz4/log.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
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

auto VmAspace::kernel_aspace() -> ustl::Arc<VmAspace> {
    return ustl::Arc<VmAspace>::adopt(KERNEL_ASPACE);
}

// Initializer order follows the member declaration order in vm_aspace.hpp.
// m_refcnt is owned by the ustl::RefCounted base (initialized by Base()), and
// m_num_users belongs to VmPage, not here -- both were leftovers.
VmAspace::VmAspace(VirtAddr base, usize size, VmasFlags flags, char const *name)
    : Base(), m_flags(flags), m_base(base), m_size(size), m_arch(base, size, flags), m_root_vma() {}

VmAspace::~VmAspace() {}

auto VmAspace::create(VmasFlags flags, char const *name, ustl::Arc<VmAspace> *out) -> Status {
    VirtAddr base;
    usize size;
    if (bool(flags & VmasFlags::User)) {
        base = USER_ASPACE_BASE;
        size = USER_ASPACE_SIZE;
    } else {
        base = KERNEL_ASPACE_BASE;
        size = KERNEL_ASPACE_SIZE;
    }

    return Self::create(base, size, flags, name, out);
}

auto VmAspace::create(VirtAddr base, usize size, VmasFlags flags, char const *name,
                      ustl::Arc<VmAspace> *out) -> Status {
    auto const aspace = AspaceCache::get(GAF_KERNEL, base, size, flags, name);
    if (!aspace) {
        return Status::OutOfMem;
    }

    aspace->init();
    {
        // Entrollment into the global list.
        Guard guard{ALL_ASPACE_LIST_LOCK};
        ALL_ASPACE_LIST.push_back(*aspace);
    }
    *out = ustl::Arc<VmAspace>::adopt(aspace);
    return Status::Ok;
}

INIT_CODE auto VmAspace::init_kernel_aspace() -> void {
    constexpr auto flags = VmasFlags::Kernel;

    auto status = AspaceCache::init("aspace-cache", ObjectCache::Flags::Folio);
    if (Status::Ok != status) {
        panic("Failed to create object cache for VmAspace");
    }

    ustl::Arc<Self> aspace;
    status = Self::create(KERNEL_ASPACE_BASE, KERNEL_ASPACE_SIZE, flags, "K:Aspace", &aspace);
    ASSERT(Status::Ok == status, "Failed to create kernel VmAspace");

    Self::KERNEL_ASPACE = aspace.into_raw();
}

auto VmAspace::sync_kernel_aspace() -> void {}

auto VmAspace::switch_aspace(Self *prev, Self *next) -> void {
    ArchVmAspace::switch_context(&prev->m_arch, &next->m_arch);
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

/// Because of cyclic dependency between VmAspace and VmArea, there is not yet
/// a better way to inline it.
auto VmAspace::root_vma() -> ustl::Arc<VmArea> {
    return m_root_vma;
}

auto VmAspace::page_fault_internal(VirtAddr va, VmfCause cause, usize extra_pages) -> Status {
    m_canary.verify<Self>();

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
                m_last_fault = ustl::Arc<VmMapping>::adopt(m_root_vma->find_mapping_locked(vmf.va));
                if (!m_last_fault) {
                    return Status::NotFound;
                }
            }

            auto const [final_status, mapped] = m_last_fault->fault(&vmf);
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