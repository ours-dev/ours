#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vmm.hpp>

namespace ours::mm {
extern "C" {
VmPage *ZERO_PAGE;
PhysAddr KERNEL_PHYS_BASE;
VirtAddr KERNEL_VIRT_BASE;
}

auto is_kernel_address(VirtAddr va) -> bool {
    // The kernel half is one contiguous span, so the subtraction form
    // answers both ends at once and cannot wrap.
    return va >= KERNEL_ASPACE_BASE && (va - KERNEL_ASPACE_BASE) < KERNEL_ASPACE_SIZE;
}

auto switch_context(VmAspace *from, VmAspace *to) -> void {
    DEBUG_ASSERT(to != nullptr);
    VmAspace::switch_aspace(from, to);
}

auto set_active_aspace(VmAspace *aspace) -> void {
    DEBUG_ASSERT(aspace != nullptr);
    VmAspace::switch_aspace(nullptr, aspace);
}

auto virt_to_phys(VirtAddr virt_addr) -> PhysAddr {
    auto phys_addr = PhysMap::virt_to_phys(virt_addr);

    // TODO(SmallHuaZi): Determine if it is kernel address.

    if (!phys_addr) {
        auto status = VmAspace::kernel_aspace()->arch_aspace().query(virt_addr, &phys_addr, 0);
        if (Status::Ok != status) {
            return 0;
        }
    }

    return phys_addr;
}

auto phys_to_virt(PhysAddr phys_addr) -> VirtAddr {
    return PhysMap::phys_to_virt(phys_addr);
}

} // namespace ours::mm