#include <logz4/log.hpp>
#include <ours/arch/mp.hpp>
#include <ours/arch/x86/bootstrap.hpp>
#include <ours/arch/x86/descriptor.hpp>
#include <ours/mm/early-mem.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/platform/init.hpp>

#include <limits>

namespace ours {
NO_MANGLE u8 const kBootstrapAreaStart[] LINK_NAME("__x86_bootstrap16_start");
NO_MANGLE u8 const kBootstrapAreaEnd[] LINK_NAME("__x86_bootstrap16_end");
NO_MANGLE u8 const kX86ApEnterLongMode[] LINK_NAME("x86_ap_enter_long_mode");

NO_MANGLE u8 const kDebugSymbol0[] LINK_NAME("__debug_symbol0");
NO_MANGLE u8 const kDebugSymbol1[] LINK_NAME("__debug_symbol1");
NO_MANGLE u8 const kDebugSymbol2[] LINK_NAME("__debug_symbol2");
NO_MANGLE u8 const kDebugSymbol3[] LINK_NAME("__debug_symbol3");
NO_MANGLE u8 const kDebugSymbol4[] LINK_NAME("__debug_symbol4");

static PhysAddr s_bootstrap16_buffer_base = 0;

INIT_CODE
auto set_bootstrap16_buffer(PhysAddr base) -> void {
    using ustl::mm::is_aligned;
    ASSERT(is_aligned(base, PAGE_SIZE));
    ASSERT(base <= KB(64) - kMinBootstrap16BufferSize);

    s_bootstrap16_buffer_base = base;
}

INIT_CODE
auto make_bootstrap_area(BootstrapData **data) -> PhysAddr {
    DEBUG_ASSERT(data);
    DEBUG_ASSERT(s_bootstrap16_buffer_base, "There is no a given bootstrap area");

    constexpr static auto const kGdtSize = usize{X86_GDT_MAX_SELECTORS * X86_SEGMENT_SELECTOR_SIZE};
    static auto const kMaxBootstrapPhysicalAddress =
        static_cast<PhysAddr>(std::numeric_limits<u32>::max());

    auto const area_size = kBootstrapAreaEnd - kBootstrapAreaStart;
    ASSERT(area_size >= 0 && static_cast<usize>(area_size) <= PAGE_SIZE);

    auto bootstrap_area_start = mm::PhysMap::phys_to_virt<u8>(s_bootstrap16_buffer_base);
    memcpy(bootstrap_area_start, kBootstrapAreaStart, static_cast<usize>(area_size));

    // At here, the trampoline code has been copied fully.

    // Let the 2nd page store data.
    auto bootstrap_data = reinterpret_cast<BootstrapData *>(bootstrap_area_start + PAGE_SIZE);
    memset(bootstrap_data, 0, sizeof(*bootstrap_data));

    bootstrap_data->lond_mode_cs = X86_GDT_KERNEL_CODE64;
    auto const long_mode_phys_entry =
        s_bootstrap16_buffer_base +
        static_cast<PhysAddr>(kX86ApEnterLongMode - kBootstrapAreaStart);
    ASSERT(long_mode_phys_entry <= kMaxBootstrapPhysicalAddress);
    bootstrap_data->long_mode_phys_entry = static_cast<u32>(long_mode_phys_entry);
    bootstrap_data->long_mode_virt_entry = VirtAddr(x86_start_nonboot_cpu);

    log::trace("Bootstrap long code phys base=0x{:X}", bootstrap_data->long_mode_phys_entry);
    log::trace("Bootstrap long mode code virt base=0x{:X}", bootstrap_data->long_mode_virt_entry);

    auto gdt_base = mm::virt_to_phys(VirtAddr(x86_get_gdt()));
    if (gdt_base > kMaxBootstrapPhysicalAddress - (kGdtSize - 1)) {
        auto gdt_start = reinterpret_cast<u8 *>(ustl::mm::align_up(
            reinterpret_cast<VirtAddr>(bootstrap_data + sizeof(*bootstrap_data)), 16));
        ASSERT(gdt_start + kGdtSize <= bootstrap_area_start + PAGE_SIZE * 2);
        memcpy(gdt_start, x86_get_gdt(), kGdtSize);
        gdt_base =
            s_bootstrap16_buffer_base + static_cast<PhysAddr>(gdt_start - bootstrap_area_start);
    }
    ASSERT(gdt_base <= kMaxBootstrapPhysicalAddress - (kGdtSize - 1));
    bootstrap_data->gdtr_limit = static_cast<u16>(kGdtSize - 1);
    bootstrap_data->gdtr_base = static_cast<u32>(gdt_base);
    log::trace("Bootstrap GDT at 0x{:X}", gdt_base);

    if constexpr (OURS_DEBUG) {
        log::trace("Debug symbol[0] offset={:X}", kDebugSymbol0 - kBootstrapAreaStart);
        log::trace("Debug symbol[1] offset={:X}", kDebugSymbol1 - kBootstrapAreaStart);
        log::trace("Debug symbol[2] offset={:X}", kDebugSymbol2 - kBootstrapAreaStart);
        log::trace("Debug symbol[3] offset={:X}", kDebugSymbol3 - kBootstrapAreaStart);
        log::trace("Debug symbol[4] offset={:X}", kDebugSymbol4 - kBootstrapAreaStart);
    }

    auto pgd = mm::VmAspace::kernel_aspace()->arch_aspace().pgd_phys();
    if (pgd > kMaxBootstrapPhysicalAddress - (PAGE_SIZE - 1)) {
        // The third page leaves the trampoline data and any copied GDT intact.
        auto temporary_pgd = bootstrap_area_start + PAGE_SIZE * 2;
        memcpy(temporary_pgd, mm::PhysMap::phys_to_virt<u8>(pgd), PAGE_SIZE);
        pgd = s_bootstrap16_buffer_base + PAGE_SIZE * 2;
    }
    ASSERT(pgd <= kMaxBootstrapPhysicalAddress - (PAGE_SIZE - 1));
    bootstrap_data->pgd = static_cast<u32>(pgd);
    *data = bootstrap_data;
    log::trace("Bootstrap PGD at 0x{:X}", pgd);

    log::trace("Bootstrap16 data at v:0x{:X}, p:0x{:X}", VirtAddr(bootstrap_data),
               mm::PhysMap::virt_to_phys(bootstrap_data));

    auto status = mm::VmAspace::kernel_aspace()->arch_aspace().map(
        s_bootstrap16_buffer_base, s_bootstrap16_buffer_base,
        kMinBootstrap16BufferSize >> PAGE_SHIFT, mm::MmuFlags::PermMask | mm::MmuFlags::Discache,
        mm::MapControl::ErrorIfExisting, 0);
    ASSERT(Status::Ok == status);

    status = mm::VmAspace::kernel_aspace()->arch_aspace().map(
        gdt_base, gdt_base, 1, mm::MmuFlags::PermMask | mm::MmuFlags::Discache,
        mm::MapControl::ErrorIfExisting, 0);
    ASSERT(Status::Ok == status);

    return s_bootstrap16_buffer_base;
}

INIT_CODE
static auto init_bootstrap16_buffer() -> void {
    if (s_bootstrap16_buffer_base) {
        // Avoid confliction.
        return;
    }

    auto buffer =
        mm::EarlyMem::allocate(kMinBootstrap16BufferSize, PAGE_SIZE, 0, KB(64), bootmem::kAnyNode);
    ASSERT(buffer, "Failed to allocate APs's bootstrap buffer");

    set_bootstrap16_buffer(mm::PhysMap::virt_to_phys(buffer));
}
GKTL_INIT_HOOK(InitBootstrap16Buffer, init_bootstrap16_buffer, PlatformInitLevel::PrePmm);

INIT_CODE
static auto free_bootstrap16_buffer() -> void {
    for (auto offset = usize{0}; offset < kMinBootstrap16BufferSize; offset += PAGE_SIZE) {
        auto const frame = mm::phys_to_frame(s_bootstrap16_buffer_base + offset);
        mm::free_frames(frame, 0);
    }
}
GKTL_INIT_HOOK(FreeBootstrap16Buffer, free_bootstrap16_buffer, gktl::InitLevel::Platform);

} // namespace ours
