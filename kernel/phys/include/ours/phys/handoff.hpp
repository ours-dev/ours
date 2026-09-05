/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_HANDOFF_HPP
#define OURS_PHYS_HANDOFF_HPP 1

#include <ours/assert.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/phys/arch_bootmem.hpp>
#include <ours/phys/arch_handoff.hpp>
#include <ours/phys/kernel_package_handoff.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/views/span.hpp>

namespace ours::phys {
struct MemoryHandoff {
    /// The final memory map physload/physboot built during early boot: one
    /// entry per maximal run of usable RAM, each flagged as still free or
    /// already carved out for something (kernel image, page tables, the
    /// OBI package, BootMem's own bookkeeping, ...).  This is data, not a
    /// live allocator -- vmours does not share physload's BootMem instance
    /// or reach back into phys in any way; it reads this map once and
    /// builds its own physical-memory bookkeeping from it, the same way
    /// physload itself builds BootMem from an OBI memory item or a legacy
    /// platform's memory map (see bootmem::PhysicalMemory::snapshot()).
    ustl::views::Span<bootmem::MemRange const> ranges;
    PhysAddr kernel_load_addr;
};

enum class UartKind : u8 {
    /// No debug UART known (or the platform has none).
    None,

    /// A 16550-compatible UART reached through x86 port-mapped I/O -- the
    /// traditional COM1 at 0x3F8, already activate()'d by whichever
    /// Console found it (kernel/arch/x86/phys/serial_console.cpp).
    X86Pio16550,
};

struct UartHandoff {
    UartKind kind{UartKind::None};

    /// The I/O port (X86Pio16550) or physical MMIO base, depending on
    /// kind.  Meaningless when kind is None.
    u64 base{};
};

/// Everything physload/physboot hand off to vmours at boot time.  Modeled
/// after Zircon's PhysHandoff: a value vmours reads once during early init
/// and either copies into its own permanent state or discards -- never a
/// live object it keeps calling back into phys through.  Scoped down from
/// Zircon's version to what this kernel actually needs: no vDSO/userboot/
/// VMO-VMAR handoff (this isn't bootstrapping a ZBI-style userland yet),
/// no boot-instrumentation timestamps or entropy pool (nothing here reads
/// them). BootOptions/the command line aren't carried either -- vmours can
/// already re-derive both from boot_package (ours::boot::boot_package()),
/// so duplicating them here would just be two copies of the same answer.
/// CPU topology is the same story: kernel/platform/pc/interrupt.cpp
/// already walks the MADT itself once it has acpi_rsdp (below), so a
/// second, phys-computed copy of the same answer doesn't belong here
/// either -- unlike the debug UART, which vmours cannot rediscover on its
/// own (it is not a self-describing device; something has to already know
/// which port is live and already configured, or logging before ACPI is
/// up has nowhere to go).
struct Handoff {
    static constexpr auto MAGIC = 0xA1B2C3D4;

    FORCE_INLINE
    auto verify() const -> bool {
        return magic == MAGIC && !mem.ranges.empty() && boot_package.valid();
    }

    u32 const magic = MAGIC;

    /// Physical address of the ACPI RSDP, if the platform has ACPI at all.
    ustl::Option<PhysAddr> acpi_rsdp;

    /// Physical address of the EFI system table, if boot-shim came up
    /// through UEFI.  Always None today: the only boot-shim that exists
    /// (kernel/arch/x86/phys/bootshim) is Multiboot2/BIOS-only.
    ustl::Option<PhysAddr> efi_system_table;

    MemoryHandoff mem;
    BootPackageHandoff boot_package;

    /// The console vmours can start writing to immediately, before its own
    /// platform init (ACPI, drivers, ...) has run far enough to find one
    /// itself.
    UartHandoff uart;

    /// Escape hatch for whatever one architecture needs that the fields
    /// above don't cover.  Optional, arch-defined, empty by default.
    ArchHandOff arch;
};
static_assert(sizeof(Handoff) <= PAGE_SIZE,
              "the size of Handoff must be lesser than 4096 for the zero page");

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_HANDOFF_HPP
