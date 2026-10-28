/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/phys/aspace.hpp>
#include <ours/phys/init.hpp>

#include <array>

// No arch_detect_memory() override: the boot shim turns the device tree into
// the same obi::MemoryRange list a Multiboot2 shim produces, and the default
// in kernel/phys/memory.cpp registers that list as it is.

namespace ours::phys {
namespace {
    std::array<bootmem::MemoryRegion, 100> BOOTSTRAP_MEMORY_REGIONS;
    std::array<bootmem::Reservation, 100> BOOTSTRAP_RESERVED_REGIONS;
} // namespace

BootMem BOOTMEM{{.usable = BOOTSTRAP_MEMORY_REGIONS, .reservations = BOOTSTRAP_RESERVED_REGIONS}};

/// Nothing riscv-specific to reserve. What phys must keep off is already in
/// BootMem when this runs: its own image and the boot package, which
/// MemoryProvisioner reserves, and what the boot shim reports as reserved --
/// the firmware's memory, the device tree blob and the shim's own image,
/// whose stack phys still runs on.
auto arch_init_memory(Aspace *const aspace) -> void {
    if (aspace != nullptr) {
        arch_setup_aspace(*aspace);
    }
}

} // namespace ours::phys
