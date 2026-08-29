/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_ACPI_HPP
#define OURS_PLATFORM_ACPI_HPP 1

#include <acpi/table_set.hpp>

namespace ours {
/// The platform's ACPI tables, or nullptr when firmware gave us none.
///
/// Non-const because acpi::TableSet memoises its signature lookups.  Every
/// caller runs during single threaded init, which is what that cache assumes.
FORCE_INLINE inline auto get_acpi_tables() -> acpi::TableSet * {
    extern acpi::TableSet *g_acpi_tables;
    return g_acpi_tables;
}

auto acpi_init_numa() -> Status;

} // namespace ours

#endif // #ifndef OURS_PLATFORM_ACPI_HPP
