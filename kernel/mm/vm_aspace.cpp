#include <gktl/counter.hpp>
#include <gktl/init_hook.hpp>
#include <ktl/new.hpp>
#include <logz4/log.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/object-cache.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ustl/lazy_init.hpp>
#include <ustl/sync/lockguard.hpp>

namespace {
GKTL_COUNTER(vm_aspace_high_priority, "vm.aspace.high_priority")
GKTL_COUNTER(vm_aspace_accessed_harvests_performed, "vm.aspace.accessed_harvest.performed")
GKTL_COUNTER(vm_aspace_accessed_harvests_skipped, "vm.aspace.accessed_harvest.skipped")
GKTL_COUNTER(vm_aspace_last_fault_hit, "vm.aspace.last_fault.hit");
GKTL_COUNTER(vm_aspace_last_fault_miss, "vm.aspace.last_fault.miss");
} // namespace

namespace ours::mm {
using AspaceCache = mm::ObjectCache::Instance<VmAspace>;

decltype(VmAspace::g_all_aspace_list) VmAspace::g_all_aspace_list;
decltype(VmAspace::g_all_aspace_list_lock) VmAspace::g_all_aspace_list_lock;

decltype(VmAspace::g_kernel_aspace) VmAspace::g_kernel_aspace;
decltype(VmAspace::g_kernel_aspace_lock) VmAspace::g_kernel_aspace_lock;

VmAspace::VmAspace(VirtAddr base, usize size, VmasFlags flags, char const *name)
    : Base()
    , m_base(base)
    , m_size(size)
    , m_flags(flags)
    , m_arch(base, size, flags)
    , m_num_users()
    , m_refcnt(1)
    , m_root_vma() {}

VmAspace::~VmAspace() {}

auto VmAspace::create(VmasFlags flags, char const *name, ustl::Arc<VmAspace> *out) -> Status {
    VirtAddr base;
    usize size;
    if (bool(flags & VmasFlags::User)) {
        base = USER_ASPACE_BASE;
        size = USER_ASPACE_SIZE;
    }

    return Self::create(base, size, flags, name, out);
}

auto VmAspace::create(VirtAddr base, usize size, VmasFlags flags, char const *name,
                      ustl::Arc<VmAspace> *out) -> Status {
    auto const aspace = AspaceCache::get(kGafKernel, base, size, flags, name);
    if (!aspace) {
        return Status::OutOfMem;
    }

    aspace->init();
    {
        // Entrollment into the global list.
        Guard guard{g_all_aspace_list_lock};
        g_all_aspace_list.push_back(*aspace);
    }
    *out = ustl::Arc<VmAspace>::adopt(aspace);
    return Status::Ok;
}

INIT_CODE
auto VmAspace::init_kernel_aspace() -> void {
    constexpr auto const flags = VmasFlags::Kernel;

    auto status = AspaceCache::init("aspace-cache", ObjectCache::Flags::Folio);
    if (Status::Ok != status) {
        panic("Failed to create object cache for VmAspace");
    }

    ustl::Arc<Self> aspace;
    status = Self::create(KERNEL_ASPACE_BASE, KERNEL_ASPACE_SIZE, flags, "K:Aspace", &aspace);
    ASSERT(Status::Ok == status, "Failed to create kernel VmAspace");

    Self::g_kernel_aspace = aspace.into_raw();
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
    MultiPageRequest page_request;
    do {
        u32 num_mapped = 0;
        {
            // Grabs the aspace's lock to prevent another operation on it from users.
            Guard guard{get_lock()};

            if (m_last_fault->check_range(va, 1)) [[likely]] {
                vm_aspace_last_fault_hit.add(1);
            } else {
                vm_aspace_last_fault_miss.add(1);
                m_last_fault = m_root_vma->find_mapping_locked(va);
                if (!m_last_fault) {
                    return Status::NotFound;
                }
            }

            auto const [final_status, mapped] = m_last_fault->fault(0);
            status = final_status;
            num_mapped = mapped;
        }

        // There is no sufficient pages to map, so we need to wait for the page
        // request to be fulfilled.
        if (Status::ShouldWait == status) {
            auto const waited_status = page_request.wait(!!(cause & VmfCause::User));
            if (Status::Ok != waited_status) {
                if (Status::TimeOut == waited_status) {
                    m_root_vma->dump_locked();
                }
                return waited_status;
            }

            if (num_mapped > 0) {
                va += num_mapped * PAGE_SIZE;

                DEBUG_ASSERT(extra_pages >= num_mapped);
                extra_pages -= num_mapped;
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