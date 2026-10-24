#include <ours/arch/mp.hpp>
#include <ours/arch/x86/bootstrap.hpp>
#include <ours/arch/x86/descriptor.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/platform/init.hpp>

#include <limits>

#include <logz4/log.hpp>

namespace ours {
extern "C" u8 const BOOTSTRAP_AREA_START[] LINK_NAME("__x86_bootstrap16_start");
extern "C" u8 const BOOTSTRAP_AREA_END[] LINK_NAME("__x86_bootstrap16_end");
extern "C" u8 const X86_AP_ENTER_LONG_MODE[] LINK_NAME("x86_ap_enter_long_mode");

extern "C" u8 const DEBUG_SYMBOL0[] LINK_NAME("__debug_symbol0");
extern "C" u8 const DEBUG_SYMBOL1[] LINK_NAME("__debug_symbol1");
extern "C" u8 const DEBUG_SYMBOL2[] LINK_NAME("__debug_symbol2");
extern "C" u8 const DEBUG_SYMBOL3[] LINK_NAME("__debug_symbol3");
extern "C" u8 const DEBUG_SYMBOL4[] LINK_NAME("__debug_symbol4");

static PhysAddr BOOTSTRAP16_BUFFER_BASE = 0;

#ifdef CONFIG_CPU_HOTPLUG
/// The data page of the trampoline, once make_bootstrap_area() has built it,
/// for a CPU started again after boot.
static BootstrapData *BOOTSTRAP_DATA = nullptr;
#endif // #ifdef CONFIG_CPU_HOTPLUG

namespace {
    constexpr auto GDT_SIZE = usize{X86_GDT_MAX_SELECTORS * X86_SEGMENT_SELECTOR_SIZE};
    constexpr auto MAX_BOOTSTRAP_PHYSICAL_ADDRESS =
        static_cast<PhysAddr>(std::numeric_limits<u32>::max());

    /// Points the trampoline at the GDT and the kernel's top-level page table,
    /// as 32-bit physical addresses: where they are, or a copy below 4 GiB
    /// inside the buffer when they are above it. Returns the GDT's address.
    ///
    /// A copy is a snapshot, so it is taken again each time a CPU is started
    /// (prepare_bootstrap_area()): a top-level entry the kernel added since
    /// boot would be missing from the boot-time one.
    CPU_INIT_CODE
    auto fill_bootstrap_tables(u8 *const area, BootstrapData *const data) -> PhysAddr {
        auto gdt_base = mm::virt_to_phys(VirtAddr(x86_get_gdt()));
        if (gdt_base > MAX_BOOTSTRAP_PHYSICAL_ADDRESS - (GDT_SIZE - 1)) {
            auto gdt_start = reinterpret_cast<u8 *>(
                ustl::mm::align_up(reinterpret_cast<VirtAddr>(data + sizeof(*data)), 16));
            ASSERT(gdt_start + GDT_SIZE <= area + PAGE_SIZE * 2);
            memcpy(gdt_start, x86_get_gdt(), GDT_SIZE);
            gdt_base = BOOTSTRAP16_BUFFER_BASE + static_cast<PhysAddr>(gdt_start - area);
        }
        ASSERT(gdt_base <= MAX_BOOTSTRAP_PHYSICAL_ADDRESS - (GDT_SIZE - 1));
        data->gdtr_limit = static_cast<u16>(GDT_SIZE - 1);
        data->gdtr_base = static_cast<u32>(gdt_base);

        auto pgd = mm::VmAspace::kernel_aspace()->arch_aspace().pgd_phys();
        if (pgd > MAX_BOOTSTRAP_PHYSICAL_ADDRESS - (PAGE_SIZE - 1)) {
            // The third page leaves the trampoline data and any copied GDT intact.
            auto temporary_pgd = area + PAGE_SIZE * 2;
            memcpy(temporary_pgd, mm::PhysMap::phys_to_virt<u8>(pgd), PAGE_SIZE);
            pgd = BOOTSTRAP16_BUFFER_BASE + PAGE_SIZE * 2;
        }
        ASSERT(pgd <= MAX_BOOTSTRAP_PHYSICAL_ADDRESS - (PAGE_SIZE - 1));
        data->pgd = static_cast<u32>(pgd);
        return gdt_base;
    }
} // namespace

INIT_CODE
auto set_bootstrap16_buffer(PhysAddr base) -> void {
    using ustl::mm::is_aligned;
    ASSERT(is_aligned(base, PAGE_SIZE));
    ASSERT(base <= 64_kb - MIN_BOOTSTRAP16_BUFFER_SIZE);

    BOOTSTRAP16_BUFFER_BASE = base;
}

INIT_CODE
auto make_bootstrap_area(BootstrapData **data) -> PhysAddr {
    DEBUG_ASSERT(data);
    DEBUG_ASSERT(BOOTSTRAP16_BUFFER_BASE, "There is no a given bootstrap area");

    auto const area_size = BOOTSTRAP_AREA_END - BOOTSTRAP_AREA_START;
    ASSERT(area_size >= 0 && static_cast<usize>(area_size) <= PAGE_SIZE);

    auto bootstrap_area_start = mm::PhysMap::phys_to_virt<u8>(BOOTSTRAP16_BUFFER_BASE);
    memcpy(bootstrap_area_start, BOOTSTRAP_AREA_START, static_cast<usize>(area_size));

    // At here, the trampoline code has been copied fully.

    // Let the 2nd page store data.
    auto bootstrap_data = reinterpret_cast<BootstrapData *>(bootstrap_area_start + PAGE_SIZE);
    memset(bootstrap_data, 0, sizeof(*bootstrap_data));

    bootstrap_data->lond_mode_cs = X86_GDT_KERNEL_CODE64;
    auto const long_mode_phys_entry =
        BOOTSTRAP16_BUFFER_BASE +
        static_cast<PhysAddr>(X86_AP_ENTER_LONG_MODE - BOOTSTRAP_AREA_START);
    ASSERT(long_mode_phys_entry <= MAX_BOOTSTRAP_PHYSICAL_ADDRESS);
    bootstrap_data->long_mode_phys_entry = static_cast<u32>(long_mode_phys_entry);
    bootstrap_data->long_mode_virt_entry = VirtAddr(x86_start_nonboot_cpu);

    log::trace("Bootstrap long code phys base=0x{:X}", bootstrap_data->long_mode_phys_entry);
    log::trace("Bootstrap long mode code virt base=0x{:X}", bootstrap_data->long_mode_virt_entry);

    auto const gdt_base = fill_bootstrap_tables(bootstrap_area_start, bootstrap_data);
    log::trace("Bootstrap GDT at 0x{:X}", gdt_base);

    if constexpr (KERNEL_DEBUG_LEVEL) {
        log::trace("Debug symbol[0] offset={:X}", DEBUG_SYMBOL0 - BOOTSTRAP_AREA_START);
        log::trace("Debug symbol[1] offset={:X}", DEBUG_SYMBOL1 - BOOTSTRAP_AREA_START);
        log::trace("Debug symbol[2] offset={:X}", DEBUG_SYMBOL2 - BOOTSTRAP_AREA_START);
        log::trace("Debug symbol[3] offset={:X}", DEBUG_SYMBOL3 - BOOTSTRAP_AREA_START);
        log::trace("Debug symbol[4] offset={:X}", DEBUG_SYMBOL4 - BOOTSTRAP_AREA_START);
    }
    log::trace("Bootstrap PGD at 0x{:X}", bootstrap_data->pgd);
    *data = bootstrap_data;
#ifdef CONFIG_CPU_HOTPLUG
    BOOTSTRAP_DATA = bootstrap_data;
#endif // #ifdef CONFIG_CPU_HOTPLUG

    log::trace("Bootstrap16 data at v:0x{:X}, p:0x{:X}", VirtAddr(bootstrap_data),
               mm::PhysMap::virt_to_phys(bootstrap_data));

    auto status = mm::VmAspace::kernel_aspace()->arch_aspace().map(
        BOOTSTRAP16_BUFFER_BASE, BOOTSTRAP16_BUFFER_BASE, MIN_BOOTSTRAP16_BUFFER_SIZE >> PAGE_SHIFT,
        mm::MmuFlags::PermMask | mm::MmuFlags::Discache, mm::ArchMapOptions::ErrorIfExisting, 0);
    ASSERT(Status::Ok == status);

    status = mm::VmAspace::kernel_aspace()->arch_aspace().map(
        gdt_base, gdt_base, 1, mm::MmuFlags::PermMask | mm::MmuFlags::Discache,
        mm::ArchMapOptions::ErrorIfExisting, 0);
    ASSERT(Status::Ok == status);

    return BOOTSTRAP16_BUFFER_BASE;
}

INIT_CODE
static auto init_bootstrap16_buffer() -> void {
    if (BOOTSTRAP16_BUFFER_BASE) {
        // Avoid confliction.
        return;
    }

    auto buffer = mm::EarlyMem::allocate(MIN_BOOTSTRAP16_BUFFER_SIZE, PAGE_SIZE, 0, 64_kb,
                                         bootmem::ANY_NODE, bootmem::MemoryType::Transient);
    ASSERT(buffer, "Failed to allocate APs's bootstrap buffer");

    set_bootstrap16_buffer(mm::PhysMap::virt_to_phys(buffer));
}
GKTL_INIT_HOOK(InitBootstrap16Buffer, init_bootstrap16_buffer, PlatformInitLevel::PrePmm);

#ifdef CONFIG_CPU_HOTPLUG
CPU_INIT_CODE
auto prepare_bootstrap_area(CpuNum const cpu, VirtAddr const stack_top, VirtAddr const thread)
    -> PhysAddr {
    ASSERT(BOOTSTRAP_DATA != nullptr, "The trampoline was never built");
    auto *const area = mm::PhysMap::phys_to_virt<u8>(BOOTSTRAP16_BUFFER_BASE);
    auto _ = fill_bootstrap_tables(area, BOOTSTRAP_DATA);
    BOOTSTRAP_DATA->cpunum = cpu;
    BOOTSTRAP_DATA->stack_top = stack_top;
    BOOTSTRAP_DATA->thread = thread;
    return BOOTSTRAP16_BUFFER_BASE;
}
#else
/// Without CPU hotplug no CPU starts after boot, so the buffer goes back once
/// every one has. Its identity mapping is left: nothing reaches it.
INIT_CODE
static auto free_bootstrap16_buffer() -> void {
    for (auto offset = usize{0}; offset < MIN_BOOTSTRAP16_BUFFER_SIZE; offset += PAGE_SIZE) {
        auto const frame = mm::phys_to_frame(BOOTSTRAP16_BUFFER_BASE + offset);
        mm::free_frames(frame, 0);
    }
}
GKTL_INIT_HOOK(FreeBootstrap16Buffer, free_bootstrap16_buffer, gktl::InitLevel::Platform);
#endif // #ifdef CONFIG_CPU_HOTPLUG

} // namespace ours
