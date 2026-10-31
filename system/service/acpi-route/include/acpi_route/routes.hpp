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
/// `interrupt_map` takes, and the trigger and polarity are what the mode of
/// `interrupt_create` has to say.
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
/// interrupt source overrides: the first override of the ISA bus that names
/// it, or, when there is none, the same number as a GSI, edge triggered and
/// active high, which is what an ISA line is.
///
/// NotFound for a number that is no ISA line (16 and up) or is the cascade, and
/// for tables with no MADT. InvArgs when the override says something this
/// cannot honour (a reserved polarity or trigger mode), and when the MADT's
/// entries end in a malformed one before an override of `isa_irq` was found:
/// a table that cannot be read to its end cannot say there is no override.
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
