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

    // What the tables say nothing against: the line is its own number as a GSI,
    // wired as an ISA line is, edge triggered and active high.
    auto route = InterruptRoute{
        .gsi = isa_irq,
        .trigger = Trigger::Edge,
        .polarity = Polarity::ActiveHigh,
    };

    // The rules are the kernel's, so that the line named here is the pin the
    // kernel programmed for the ISA number: the kernel keeps one override per
    // ISA number, a later entry replacing an earlier, so the LAST override of
    // the number wins; it does not read the bus field of an entry, which the
    // MADT fixes at 0, so neither does this; and its walk ends at the first
    // entry that is malformed and does not look at what is left, so this one
    // ends there too, and what was seen stands.
    for (auto const &entry: acpi::entries<acpi::madt::InterruptSourceOverrideEntry>(*madt)) {
        if (entry.source != isa_irq) {
            continue;
        }

        // "Conforms" is the bus's own: an ISA line is edge triggered and
        // active high.
        route = InterruptRoute{
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
    }
    return ustl::ok(route);
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
