/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// DBG2 decoding.  These paths had no implementation at all: debug.cpp was a
/// zero byte file while debug.hpp declared two functions and BUILD.gn listed
/// the file, so any caller got a link error.

#include "acpi_test_blob.hpp"
#include <acpi/debug.hpp>

#include <cstdlib>
#include <cstring>

namespace {

using acpi::test::Machine;
using acpi::test::require;

struct Dbg2Layout {
    acpi::dbg2::Table *table;
    acpi::dbg2::Device *device;
    acpi::GenericAddress *address;
    u32 *address_size;
};

constexpr usize DEVICE_OFFSET = sizeof(acpi::dbg2::Table);
constexpr usize ADDRESS_OFFSET = sizeof(acpi::dbg2::Device);
constexpr usize ADDRESS_SIZE_OFFSET = ADDRESS_OFFSET + sizeof(acpi::GenericAddress);
constexpr usize DEVICE_LENGTH = ADDRESS_SIZE_OFFSET + sizeof(u32);

auto add_dbg2(Machine &machine) -> Dbg2Layout {
    auto const length = DEVICE_OFFSET + DEVICE_LENGTH;
    auto *table = machine.add_table<acpi::dbg2::Table>(acpi::dbg2::Table::SIGNATURE, length);
    table->device_info_offset = u32(DEVICE_OFFSET);
    table->device_info_count = 1;

    auto *bytes = reinterpret_cast<u8 *>(table);
    auto *device = reinterpret_cast<acpi::dbg2::Device *>(bytes + DEVICE_OFFSET);
    device->revision = 0;
    device->length = u16(DEVICE_LENGTH);
    device->register_count = 1;
    device->port_type = acpi::dbg2::PortType::Serial;
    device->port_subtype = acpi::dbg2::PortSubType::Serial16550Compatible;
    device->base_address_offset = u16(ADDRESS_OFFSET);
    device->address_size_offset = u16(ADDRESS_SIZE_OFFSET);

    auto *address =
        reinterpret_cast<acpi::GenericAddress *>(bytes + DEVICE_OFFSET + ADDRESS_OFFSET);
    address->address_space_id = u8(acpi::AddressSpace::SystemMemory);
    address->register_bit_width = 32;
    address->address = 0xfedc'0000;

    auto *address_size = reinterpret_cast<u32 *>(bytes + DEVICE_OFFSET + ADDRESS_SIZE_OFFSET);
    *address_size = 0x1000;

    return Dbg2Layout{table, device, address, address_size};
}

auto test_mmio_port() -> void {
    Machine machine;
    auto layout = add_dbg2(machine);

    auto port = acpi::parse_debug_port(*layout.table);
    require(bool(port));
    require(port->kind == acpi::DebugPort::Kind::Mmio);
    require(port->address == 0xfedc'0000);
    require(port->length == 0x1000);
}

auto test_port_io() -> void {
    Machine machine;
    auto layout = add_dbg2(machine);
    layout.address->address_space_id = u8(acpi::AddressSpace::SystemIo);
    layout.address->address = 0x3f8;

    auto port = acpi::parse_debug_port(*layout.table);
    require(bool(port));
    require(port->kind == acpi::DebugPort::Kind::PortIo);
    require(port->address == 0x3f8);
}

auto test_non_serial_is_not_found() -> void {
    Machine machine;
    auto layout = add_dbg2(machine);
    layout.device->port_type = acpi::dbg2::PortType::Usb;
    require(!acpi::parse_debug_port(*layout.table));

    layout.device->port_type = acpi::dbg2::PortType::Serial;
    layout.device->port_subtype = acpi::dbg2::PortSubType::SerialArmPl011;
    require(!acpi::parse_debug_port(*layout.table));
}

auto test_malformed_offsets_are_rejected() -> void {
    Machine machine;
    auto layout = add_dbg2(machine);

    // A device offset pointing back into the table header.
    layout.table->device_info_offset = 4;
    require(!acpi::parse_debug_port(*layout.table));
    layout.table->device_info_offset = u32(DEVICE_OFFSET);

    // A device claiming to be longer than the table.
    layout.device->length = 0xffff;
    require(!acpi::parse_debug_port(*layout.table));
    layout.device->length = u16(DEVICE_LENGTH);

    // A base address offset that would read past the device.
    layout.device->base_address_offset = u16(DEVICE_LENGTH - 4);
    require(!acpi::parse_debug_port(*layout.table));
    layout.device->base_address_offset = u16(ADDRESS_OFFSET);

    // A device count larger than the table can hold.  The first device has to be
    // one the decoder skips, otherwise it returns a port before it ever looks at
    // the bogus count.
    layout.device->port_type = acpi::dbg2::PortType::Usb;
    layout.table->device_info_count = 4;
    require(!acpi::parse_debug_port(*layout.table));
    layout.table->device_info_count = 1;
    layout.device->port_type = acpi::dbg2::PortType::Serial;

    require(bool(acpi::parse_debug_port(*layout.table)));
}

auto test_missing_address_size_falls_back_to_bit_width() -> void {
    Machine machine;
    auto layout = add_dbg2(machine);
    // Point the size array past the end of the device: the decoder falls back
    // to the width the generic address itself declares.
    layout.device->address_size_offset = u16(DEVICE_LENGTH);

    auto port = acpi::parse_debug_port(*layout.table);
    require(bool(port));
    require(port->length == 4);
}

} // namespace

auto main() -> int {
    test_mmio_port();
    test_port_io();
    test_non_serial_is_not_found();
    test_malformed_offsets_are_rejected();
    test_missing_address_size_falls_back_to_bit_width();
    return 0;
}
