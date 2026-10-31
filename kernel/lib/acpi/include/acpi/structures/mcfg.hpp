/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_STRUCTURES_MCFG_HPP
#define ACPI_STRUCTURES_MCFG_HPP 1

#include <acpi/structures/header.hpp>

namespace acpi::mcfg {
/// PCI Express memory mapped configuration space base address description
/// table, "MCFG": the header, eight reserved bytes, then `Allocation`s up to
/// the table's `header.length`.
///
/// Reference: the PCI Firmware Specification's MCFG table description
/// (section 4.1.2 in the 3.x revisions; the number is from memory).
struct PACKED Table {
    [[nodiscard]]
    constexpr auto size() const -> u32 {
        return header.length;
    }

    TableHeader header;
    u64 reserved;

    constexpr static Signature SIGNATURE = Signature::from_chars("MCFG");
};
static_assert(sizeof(Table) == 44);

/// One memory mapped configuration space of the table: the buses
/// `start_bus` to `end_bus` of PCI segment group `segment_group`.
///
/// `base_address` is where the configuration space of bus 0 would begin, one
/// MiB per bus, whether or not the segment has a bus 0: the space of bus N is
/// at `base_address + (N << 20)`.
struct PACKED Allocation {
    u64 base_address;
    u16 segment_group;
    u8 start_bus;
    u8 end_bus;
    u32 reserved;
};
static_assert(sizeof(Allocation) == 16);

} // namespace acpi::mcfg

#endif // #ifndef ACPI_STRUCTURES_MCFG_HPP
