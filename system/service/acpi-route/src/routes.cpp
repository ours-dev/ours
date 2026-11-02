/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <cstring>

#include <acpi/structures/madt.hpp>
#include <acpi/structures/mcfg.hpp>
#include <acpi/table_set.hpp>
#include <acpi_route/routes.hpp>

namespace acpi_route {
namespace {
    using ours::Status;

    /// The MADT names the ISA bus as bus 0 in an interrupt source override.
    constexpr auto ISA_BUS = u8{0};

    /// One MiB of configuration space to a bus.
    constexpr auto BUS_SHIFT = 20;
} // namespace

auto isa_irq_to_gsi(TableDirectory const &tables, u32 const isa_irq)
    -> ustl::Result<InterruptRoute, Status> {
    if (isa_irq >= NUM_ISA_IRQS || isa_irq == ISA_IRQ_CASCADE) {
        return ustl::err(Status::NotFound);
    }
    auto const *madt = tables.find<acpi::madt::Table>();
    if (madt == nullptr) {
        return ustl::err(Status::NotFound);
    }

    auto const entries = acpi::entries<acpi::madt::InterruptSourceOverrideEntry>(*madt);
    for (auto const &entry: entries) {
        if (entry.bus != ISA_BUS || entry.source != isa_irq) {
            continue;
        }

        // "Conforms" is the bus's own: an ISA line is edge triggered and
        // active high.
        auto route = InterruptRoute{
            .gsi = entry.global_irq,
            .trigger = Trigger::Edge,
            .polarity = Polarity::ActiveHigh,
        };
        switch (entry.trigger_mode()) {
            case acpi::madt::TriggerMode::Conforms:
            case acpi::madt::TriggerMode::Edge:
                break;
            case acpi::madt::TriggerMode::Level:
                route.trigger = Trigger::Level;
                break;
            default:
                return ustl::err(Status::InvArgs);
        }
        switch (entry.polarity()) {
            case acpi::madt::Polarity::Conforms:
            case acpi::madt::Polarity::ActiveHigh:
                break;
            case acpi::madt::Polarity::ActiveLow:
                route.polarity = Polarity::ActiveLow;
                break;
            default:
                return ustl::err(Status::InvArgs);
        }
        return ustl::ok(route);
    }

    if (entries.malformed()) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(InterruptRoute{
        .gsi = isa_irq,
        .trigger = Trigger::Edge,
        .polarity = Polarity::ActiveHigh,
    });
}

auto ecam_window(TableDirectory const &tables, u16 const segment)
    -> ustl::Result<EcamWindow, Status> {
    auto const *mcfg = tables.find<acpi::mcfg::Table>();
    if (mcfg == nullptr) {
        return ustl::err(Status::NotFound);
    }

    // A partial allocation at the end of the table is not one.
    auto const count =
        (usize{mcfg->header.length} - sizeof(acpi::mcfg::Table)) / sizeof(acpi::mcfg::Allocation);
    auto const *first = reinterpret_cast<u8 const *>(mcfg) + sizeof(acpi::mcfg::Table);
    for (auto index = usize{0}; index < count; ++index) {
        acpi::mcfg::Allocation allocation;
        std::memcpy(&allocation, first + index * sizeof(allocation), sizeof(allocation));
        if (allocation.segment_group != segment) {
            continue;
        }
        if (allocation.end_bus < allocation.start_bus) {
            return ustl::err(Status::InvArgs);
        }

        auto const size = (u64{allocation.end_bus} - allocation.start_bus + 1) << BUS_SHIFT;
        auto const address = allocation.base_address + (u64{allocation.start_bus} << BUS_SHIFT);
        if (address < allocation.base_address || address + size < address) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(EcamWindow{
            .address = address,
            .size = size,
            .segment = segment,
            .first_bus = allocation.start_bus,
            .last_bus = allocation.end_bus,
        });
    }
    return ustl::err(Status::NotFound);
}

} // namespace acpi_route
