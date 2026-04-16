/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_APIC_HPP
#define ACPI_APIC_HPP 1

#include <acpi/parser.hpp>
#include <acpi/structures/madt.hpp>

#include <ustl/function/fn.hpp>

namespace acpi {
    // The following functions is particular to x86 architecture.

    using IoApicCommitFn = ustl::function::Fn<auto(madt::IoApicEntry const &) -> Status>;
    auto enumerate_io_apics(IAcpiParser const &parser, IoApicCommitFn const &commit) -> Status;

    using LocalApicCommitFn = ustl::function::Fn<auto(madt::LocalApicEntry const &) -> Status>;
    auto enumerate_local_apics(IAcpiParser const &parser, LocalApicCommitFn const &commit) -> Status;

    using InterruptOverrideCommitFn = ustl::function::Fn<auto(madt::InterruptOverrideEntry const &) -> Status>;
    auto enumerate_interrupt_overrides(IAcpiParser const &parser, InterruptOverrideCommitFn const &commit) -> Status;

} // namespace acpi

#endif // #ifndef ACPI_APIC_HPP