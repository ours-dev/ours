/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ACPI_HPP
#define OURS_PHYS_ACPI_HPP 1

#include <acpi/table_set.hpp>

namespace ours::phys {
auto make_acpi_table_set(PhysAddr rsdp) -> ktl::Result<acpi::TableSet>;

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_ACPI_HPP
