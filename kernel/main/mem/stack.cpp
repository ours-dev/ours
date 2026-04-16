#include <ours/mem/stack.hpp>
#include <ours/mem/vm_aspace.hpp>
#include <ours/mem/vm_area.hpp>
#include <ours/mem/vm_object_paged.hpp>

#include <logz4/log.hpp>

namespace ours::mem {
    // This is relative with the boot stack, so marked INIT_DATA
    NO_MANGLE INIT_DATA 
    VirtAddr g_kernel_stack_bottom = 0;

    NO_MANGLE INIT_DATA 
    usize g_kernel_stack_size = 0;

    auto Stack::init_first() -> void {
        m_base = g_kernel_stack_bottom;
        m_size = g_kernel_stack_size;
    }

    auto Stack::init(usize size) -> Status {
        ustl::Rc<VmObjectPaged> vmo;
        auto status = VmObjectPaged::create(kGafKernel, size, VmoFlags::Pinned, &vmo);
        if (Status::Ok != status) {
            log::trace("Failed to allocate a kernel stack, size={}", size);
            return status;
        }
        vmo->set_name("k-stack");

        auto rvma = VmAspace::kernel_aspace()->root_vma();
        DEBUG_ASSERT(rvma);

        ustl::Rc<VmMapping> mapping;
        status = rvma->create_mapping(0, size, 0,
            MmuFlags::Readable | MmuFlags::Writable, 
            vmo, 
            "k-stack-mapping", 
            VmMapOption::Pinned | VmMapOption::Commit,
            &mapping
        );
        if (Status::Ok != status) {
            log::trace("Failed to allocate a VmMapping, size={}, reason={}", size, to_string(status));
            return status;
        }

        status = mapping->map(0, size, true, MapControl::ErrorIfExisting);
        if (Status::Ok != status) {
            log::trace("Failed to map the space for kernel stack, size={}, reason={}", size, to_string(status));
            return status;
        }

        m_stack = ustl::move(mapping);
        m_base = m_stack->base();
        m_size = m_stack->size();

        log::trace("Allcoated Kernel stack:[{:X}, {:X})", m_base, top());
        return Status::Ok;
    }

} // namespace ours::mem