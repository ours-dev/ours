/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_HANDOFF_HPP
#define OURS_PHYS_HANDOFF_HPP 1

#include <ours/assert.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/phys/arch-bootmem.hpp>
#include <ours/phys/arch-handoff.hpp>
#include <ours/phys/kernel-package-handoff.hpp>
#include <ustl/views/span.hpp>

namespace ours::phys {
struct EarlyMem {
    using ReserveFn = auto (*)(void *, bootmem::Range) -> Status;
    using AssignNodeFn = auto (*)(void *, bootmem::Range, NnId) -> Status;
    using AllocateFn = auto (*)(void *, bootmem::AllocationRequest, bootmem::Range *) -> Status;
    using ReleaseFn = auto (*)(void *, bootmem::Range) -> Status;
    using VisitFn = auto (*)(void *, bootmem::MemoryQuery, bootmem::MemoryVisitor, void *)
        -> Status;

    void *context;
    ReserveFn reserve;
    AssignNodeFn assign_node;
    AllocateFn allocate;
    ReleaseFn release;
    VisitFn visit;

    PhysAddr min_address;
    PhysAddr max_address;

    [[nodiscard]]
    auto valid() const -> bool {
        return context != nullptr && reserve != nullptr && assign_node != nullptr &&
               allocate != nullptr && release != nullptr && visit != nullptr &&
               min_address < max_address;
    }
};

struct MemoryHandoff {
    EarlyMem bootmem;
    PhysAddr kernel_load_addr;
};

///
struct Handoff {
    static constexpr u32 kMagic = 0xA1B2C3D4;

    FORCE_INLINE
    auto verify() const -> bool {
        return magic == kMagic && mem.bootmem.valid() && boot_package.valid();
    }

    u32 const magic = kMagic;
    u32 nr_cpus;
    PhysAddr efi_system_table;
    PhysAddr acpi_rsdp;
    MemoryHandoff mem;
    BootPackageHandoff boot_package;
    ustl::views::Span<CpuTopologyNode> cpu_topology;

    // Optional and only to provide unusal quirks from a part of architectures, so put it here.
    ArchHandOff arch;
};
static_assert(sizeof(Handoff) <= PAGE_SIZE,
              "the size of Handoff must be lesser than 4096 for the zero page");

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_HANDOFF_HPP
