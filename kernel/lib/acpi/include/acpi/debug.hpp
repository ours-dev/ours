/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_DEBUG_HPP
#define ACPI_DEBUG_HPP 1

#include <acpi/structures/dbg2.hpp>
#include <acpi/table_set.hpp>

namespace acpi {
/// Where the firmware put the debug serial port.
struct DebugPort {
    enum class Kind : u8 {
        /// `address` is an I/O port base.
        PortIo,
        /// `address` is the physical base of the 16550 register block.
        Mmio,
    };

    Kind kind;
    PhysAddr address;
    u64 length;
};

/// Decodes the first usable 16550-compatible serial port out of a DBG2 table.
///
/// Returns NotFound when the table describes no such port.
auto parse_debug_port(dbg2::Table const &table) -> ktl::Result<DebugPort>;

/// Looks the DBG2 table up and decodes it.
auto find_debug_port(TableSet &tables) -> ktl::Result<DebugPort>;

} // namespace acpi

#endif // #ifndef ACPI_DEBUG_HPP
