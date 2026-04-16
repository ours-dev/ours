/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ACPI_HPP
#define OURS_PHYS_ACPI_HPP 1

#include <acpi/parser.hpp>

namespace ours::phys {
    auto make_apic_parser(PhysAddr rsdp) -> ktl::Result<acpi::AcpiParser>;

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_ACPI_HPP