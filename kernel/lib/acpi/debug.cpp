/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <acpi/debug.hpp>

namespace acpi {
namespace {
    /// DBG2 offsets are byte counts, so nothing in the table is guaranteed to
    /// land on a natural boundary.
    template <typename T>
    [[nodiscard]]
    auto read_unaligned(void const *src) -> T {
        T value;
        __builtin_memcpy(&value, src, sizeof(T));
        return value;
    }

    [[nodiscard]]
    auto is_16550(dbg2::Device const &device) -> bool {
        if (device.port_type != dbg2::PortType::Serial) {
            return false;
        }

        return device.port_subtype == dbg2::PortSubType::Serial16550Compatible ||
               device.port_subtype == dbg2::PortSubType::Serial16550Subset ||
               device.port_subtype == dbg2::PortSubType::Serial16550WithParameters;
    }

    /// Pulls the first register descriptor out of one device entry.  Every
    /// offset in a DBG2 device is relative to the device itself, and each is
    /// bounds checked against the device's own declared length.
    [[nodiscard]]
    auto decode_device(dbg2::Device const &device) -> ktl::Result<DebugPort> {
        auto const device_length = usize(device.size());
        if (device.register_count == 0) {
            return ktl::err(Status::NotFound);
        }

        auto const address_offset = usize(device.base_address_offset);
        if (address_offset > device_length ||
            device_length - address_offset < sizeof(GenericAddress)) {
            return ktl::err(Status::InvalidArgument);
        }

        auto const *bytes = reinterpret_cast<u8 const *>(&device);
        auto const address = read_unaligned<GenericAddress>(bytes + address_offset);

        auto port = DebugPort{};
        switch (static_cast<AddressSpace>(address.address_space_id)) {
            case AddressSpace::SystemMemory:
                port.kind = DebugPort::Kind::Mmio;
                break;
            case AddressSpace::SystemIo:
                port.kind = DebugPort::Kind::PortIo;
                break;
            default:
                return ktl::err(Status::NotSupported);
        }
        port.address = PhysAddr(address.address);

        // The address size array is optional in practice; fall back to the
        // width the generic address itself declares when it is missing.
        auto const size_offset = usize(device.address_size_offset);
        if (size_offset <= device_length && device_length - size_offset >= sizeof(u32)) {
            port.length = read_unaligned<u32>(bytes + size_offset);
        } else {
            port.length = address.register_bit_width / 8;
        }

        return ktl::ok(port);
    }
} // namespace

auto parse_debug_port(dbg2::Table const &table) -> ktl::Result<DebugPort> {
    auto const total = usize(table.size());
    if (total < sizeof(dbg2::Table)) {
        return ktl::err(Status::InvalidArgument);
    }

    auto const *bytes = reinterpret_cast<u8 const *>(&table);
    auto offset = usize(table.device_info_offset);
    for (auto i = u32(0); i < table.device_info_count; ++i) {
        if (offset < sizeof(dbg2::Table) || offset > total ||
            total - offset < sizeof(dbg2::Device)) {
            return ktl::err(Status::InvalidArgument);
        }

        auto const &device = *reinterpret_cast<dbg2::Device const *>(bytes + offset);
        auto const device_length = usize(device.size());
        if (device_length < sizeof(dbg2::Device) || device_length > total - offset) {
            return ktl::err(Status::InvalidArgument);
        }

        if (is_16550(device)) {
            if (auto port = decode_device(device)) {
                return port;
            }
        }

        offset += device_length;
    }

    return ktl::err(Status::NotFound);
}

auto find_debug_port(TableSet &tables) -> ktl::Result<DebugPort> {
    auto const *table = tables.find<dbg2::Table>();
    if (!table) {
        return ktl::err(Status::NotFound);
    }

    return parse_debug_port(*table);
}

} // namespace acpi
