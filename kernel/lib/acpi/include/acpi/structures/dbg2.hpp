/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_STRUCTURES_DBG2_HPP
#define ACPI_STRUCTURES_DBG2_HPP 1

#include <acpi/structures/header.hpp>

namespace acpi::dbg2 {
/// Reference: Microsoft Debug Port Table 2, Table 2.
enum class PortType : u16 {
    Serial = 0x8000,
    T1394 = 0x8001,
    Usb = 0x8002,
    Net = 0x8003,
};

enum class PortSubType : u16 {
    Serial16550Compatible = 0x0000,
    Serial16550Subset = 0x0001,
    SerialArmPl011 = 0x0003,
    Serial16550WithParameters = 0x0012,
    T1394Standard = 0x0000,
    UsbXhci = 0x0000,
    UsbEhci = 0x0001,
};

/// Debug port table 2.
///
/// Reference: Microsoft Debug Port Table 2, Table 1.
struct PACKED Table {
    [[nodiscard]]
    constexpr auto size() const -> u32 {
        return header.length;
    }

    TableHeader header;
    /// Byte offset, from the start of this table, of the first device entry.
    u32 device_info_offset;
    u32 device_info_count;

    constexpr static Signature kSignature = Signature::from_chars("DBG2");
};
static_assert(sizeof(Table) == 44);

/// Debug device information structure.
///
/// Every `*_offset` field is a byte offset from the start of *this* structure,
/// not from the start of the table.
struct PACKED Device {
    [[nodiscard]]
    constexpr auto size() const -> u16 {
        return length;
    }

    u8 revision;
    u16 length;
    u8 register_count;
    u16 namepath_length;
    u16 namepath_offset;
    u16 oem_data_length;
    u16 oem_data_offset;
    PortType port_type;
    PortSubType port_subtype;
    u16 reserved;
    /// Offset of an array of `register_count` GenericAddress structures.
    u16 base_address_offset;
    /// Offset of an array of `register_count` u32 address sizes.
    u16 address_size_offset;
};
static_assert(sizeof(Device) == 22);

} // namespace acpi::dbg2

#endif // #ifndef ACPI_STRUCTURES_DBG2_HPP
