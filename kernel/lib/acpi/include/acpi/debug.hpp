/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_DEBUG_HPP
#define ACPI_DEBUG_HPP 1

#include <acpi/parser.hpp>
#include <acpi/structures/dbg2t.hpp>
#include <ktl/result.hpp>

namespace acpi {
    struct DebugConfig {
        enum class Type: u8 {
            PortIo,
            Mmio,
        };

        Type type;
        // Physical address of the 16550 MMIO registers for Type::kMmio.
        // IO port base for Type::kPio.
        PhysAddr address;
        PhysAddr length;
    };

    auto get_debug_port(IAcpiParser const &parser) -> ktl::Result<DebugConfig>;

    auto parse_dbg2t(Dbg2Table const &dbg2t) -> ktl::Result<DebugConfig>;

} // namespace acpi

#endif // #ifndef ACPI_DEBUG_HPP