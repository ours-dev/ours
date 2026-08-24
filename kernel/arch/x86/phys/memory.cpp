#include <ours/arch/aspace_layout.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/acpi.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/print.hpp>

#include <acpi/numa.hpp>

#include <ours/panic.hpp>

// No arch_detect_memory() override here: boot-shim converts Multiboot2's
// memory map into the same obi::MemoryRange vocabulary a real OBI memory
// item uses (kernel/arch/x86/phys/boot-shim/shim.cpp) before ever calling
// into physload, so the arch-neutral default in kernel/phys/memory.cpp
// already does the right thing with what it is handed.

namespace ours::phys {
    static bootmem::MemoryRegion s_bootstrap_memory_regions[100];
    static bootmem::Range s_bootstrap_reserved_regions[100];

    BootMem g_bootmem {
        {.usable = s_bootstrap_memory_regions, .reservations = s_bootstrap_reserved_regions}
    };

    auto arch_init_memory(Aspace *aspace) -> void {
        auto mem = global_bootmem();

        // VGA text memory is usable in neither memory-discovery path.
        auto const vga = bootmem::Range::from(0xB0000, 0x10000);
        if (vga.is_err() || mem->reserve(vga.unwrap()) != Status::Ok) {
            panic("Failed to reserve VGA memory");
        }

        if (aspace) {
            arch_setup_aspace(*aspace);
        }
    }

} // namespace ours::phys
