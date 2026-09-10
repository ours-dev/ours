#include <ours/arch/aspace_layout.hpp>
#include <ours/panic.hpp>
#include <ours/phys/acpi.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/init.hpp>

#include <acpi/numa.hpp>
#include <logz4/log.hpp>

// No arch_detect_memory() override here: boot-shim converts Multiboot2's
// memory map into the same obi::MemoryRange vocabulary a real OBI memory
// item uses (kernel/arch/x86/phys/bootshim/shim.cpp) before ever calling
// into physload, so the arch-neutral default in kernel/phys/memory.cpp
// already does the right thing with what it is handed.

namespace ours::phys {
static bootmem::MemoryRegion BOOTSTRAP_MEMORY_REGIONS[100];
static bootmem::Range BOOTSTRAP_RESERVED_REGIONS[100];

BootMem BOOTMEM{{.usable = BOOTSTRAP_MEMORY_REGIONS, .reservations = BOOTSTRAP_RESERVED_REGIONS}};

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
