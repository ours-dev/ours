/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_ROUTE_ROUTES_HPP
#define ACPI_ROUTE_ROUTES_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <acpi_route/table_directory.hpp>
#include <ustl/result.hpp>

namespace acpi_route {
/// How an interrupt signals. Edge or level, and which level (or edge) counts.
enum class Trigger : u8 {
    Edge,
    Level,
};

enum class Polarity : u8 {
    ActiveHigh,
    ActiveLow,
};

/// Where a device's interrupt arrives and how it signals: the GSI is what
/// `interrupt_map` resolves, with the root domain the service's reply adds to
/// it, and the trigger and polarity are what the mode of `interrupt_create`
/// has to say.
struct InterruptRoute {
    u32 gsi;
    Trigger trigger;
    Polarity polarity;
};

inline constexpr auto NUM_ISA_IRQS = u32{16};

/// The ISA number of the cascade between the two interrupt controllers a PC
/// has. No device is on it.
inline constexpr auto ISA_IRQ_CASCADE = u32{2};

/// The GSI of ISA interrupt `isa_irq` and how it signals, from the MADT's
/// interrupt source overrides: the last override that names it, or, when there
/// is none, the same number as a GSI, edge triggered and active high, which is
/// what an ISA line is.
///
/// The rules are the kernel's, so that the line named is the pin the kernel
/// programmed for the number: a later override of a number replaces an
/// earlier, the entry's bus field is not read, and the walk of the entries
/// ends at the first one that is malformed, what was seen before it standing.
///
/// NotFound for a number that is no ISA line (16 and up) or is the cascade, and
/// for tables with no MADT. InvArgs when the override says something this
/// cannot honour (a reserved polarity or trigger mode).
[[nodiscard]]
auto isa_irq_to_gsi(TableDirectory const &tables, u32 isa_irq)
    -> ustl::Result<InterruptRoute, ours::Status>;

/// Where one PCI segment's configuration space is in physical memory, one MiB
/// to a bus.
struct EcamWindow {
    /// Where the configuration space of `first_bus` begins.
    u64 address;
    /// The bytes of it, up to the end of `last_bus`.
    u64 size;
    u16 segment;
    u8 first_bus;
    u8 last_bus;
};

/// The window of `segment` from the MCFG: the first allocation of the table
/// that names it. NotFound for a segment the table does not list, and for
/// tables with no MCFG. InvArgs for an allocation whose last bus is before its
/// first, or whose window would run past the end of the address space.
[[nodiscard]]
auto ecam_window(TableDirectory const &tables, u16 segment)
    -> ustl::Result<EcamWindow, ours::Status>;

} // namespace acpi_route

#endif // #ifndef ACPI_ROUTE_ROUTES_HPP
