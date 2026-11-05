/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PCI_BRIDGE_HPP
#define PCI_BRIDGE_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <pci/bar.hpp>
#include <pci/pci.hpp>
#include <ustl/result.hpp>

/// The PCI bridge's windows: what a hot-added function needs besides its
/// own BARs. A function behind a bridge is reached only through the
/// bridge's windows -- the address ranges the bridge forwards to its
/// secondary bus -- and firmware programs those at boot only, as it
/// programs the BARs: a function that arrives later arrives behind closed
/// windows, and this is what opens them. The header type-1 registers: the
/// bus numbers at 0x18, the I/O window at 0x1C, the memory window at 0x20,
/// and the command at 0x04.
namespace pci {

/// A bridge header: class base 06h, and its own page on the bus the
/// config-space view names.
constexpr u32 CLASS_BRIDGE_BASE = 0x06;

/// The bridge whose secondary bus is |bus|, over bus 0's window. NotFound
/// when bus 0 holds no bridge to it.
[[nodiscard]]
inline auto bridge_for_bus(Config &bus0, usize const bus) -> ustl::Result<usize, ours::Status> {
    for (auto index = usize{0}; index < FUNCTIONS_PER_BUS; ++index) {
        if (bus0.read16(index, 0) == 0xFFFF) {
            continue;
        }
        // The class dword's top byte is the base class: 06h names a
        // bridge, whatever its subclass.
        if ((bus0.read32(index, 8) >> 24) != CLASS_BRIDGE_BASE ||
            bus0.read8(index, 0x19) != static_cast<u8>(bus)) {
            continue;
        }
        return ustl::ok(index);
    }
    return ustl::err(ours::Status::NotFound);
}

/// Opens the bridge |index|'s memory window over [mem_base, mem_limit), and
/// names |subordinate| the highest bus behind it. The window's fields are
/// 1 MiB granular: the base and limit registers carry address bits 31:20,
/// so a window edge not on a 1 MiB line rounds the same way the decode
/// does -- the base down, the limit up, by the caller's aligned values.
/// The dword at 0x20 carries both fields, base low half, limit high half.
inline auto program_bridge_memory(Config &bus0, usize const index, u64 const mem_base,
                                  u64 const mem_limit, usize const subordinate) -> void {
    // The fields carry the window's edges in address bits 31:20: the base
    // down on its 1 MiB line, the limit up on its own -- the exclusive
    // limit's last forwarded byte is the one whose field names it.
    auto const base_field = static_cast<u32>((mem_base >> 16) & 0xFFF0u);
    auto const limit_field = static_cast<u32>(((mem_limit - 1) >> 16) & 0xFFF0u);
    auto const windows = base_field | (limit_field << 16);
    // The bus numbers dword: primary, secondary, subordinate, each a byte.
    // Secondary is the bridge's own, read here; the caller names the
    // subordinate -- the claimed function's bus -- and the primary stays.
    auto const numbers = bus0.read32(index, 0x18);
    auto const secondary = (numbers >> 8) & 0xFF;
    auto const primary = numbers & 0xFF;
    bus0.write32(index, 0x18, primary | (secondary << 8) | (static_cast<u32>(subordinate) << 16));
    bus0.write32(index, 0x20, windows);
}

/// Opens the bridge |index|'s I/O window over [io_base, io_limit). The
/// fields are 4 KiB granular and carry address bits 15:12, each byte's low
/// nibble the 32-bit-decode flag the specification keeps.
inline auto program_bridge_io(Config &bus0, usize const index, u16 const io_base,
                              u16 const io_limit) -> void {
    auto const base_field = static_cast<u32>((io_base >> 8) & 0xF0u) | 0x1u;
    auto const limit_field = static_cast<u32>(((io_limit - 1) >> 8) & 0xF0u) | 0x1u;
    bus0.write32(index, 0x1C, base_field | (limit_field << 8));
}

/// Enables the bridge: memory and I/O decode, bus master -- the command
/// word's bits, as a function's own enable is. A bridge with closed
/// windows and a clear command forwards nothing.
inline auto enable_bridge(Config &bus0, usize const index) -> void {
    auto const command = bus0.read32(index, 4);
    bus0.write32(index, 4, command | COMMAND_IO | COMMAND_MEMORY | COMMAND_MASTER);
}
} // namespace pci

#endif // #ifndef PCI_BRIDGE_HPP
