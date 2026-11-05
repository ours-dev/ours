/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PCI_BAR_HPP
#define PCI_BAR_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <array>
#include <span>

#include <pci/pci.hpp>
#include <ustl/result.hpp>

/// The Base Address Registers: how a PCI function's driver learns how much
/// address space each BAR decodes (the sizing write) and pins it to a base
/// (the assignment write). Firmware assigns BARs at boot only -- a device
/// that appears later (hot-plug) arrives with unassigned BARs, and this is
/// what lights it.
///
/// The sizing write stores all ones to a BAR and reads it back: the bits
/// that hold their value name the fixed fields (memory/IO, 64-bit, and the
/// low bits that are hardwired zero), and the bits that read zero are the
/// decode's size bits -- the size is their complement plus one, rounded to
/// the register's granularity (16 bytes for memory, 4 for I/O). The write
/// must be done with decode disabled: a live device decoding its old base
/// during the probe can answer a probe write as if it were a register.
namespace pci {

/// The two address spaces a BAR decodes.
enum class Space : u8 {
    Memory,
    Io,
};

/// One BAR, as sizing and assignment name it.
struct Bar {
    Space space = Space::Memory;
    /// Bytes the decode covers (memory), or the I/O ports' span.
    u64 size = 0;
    /// The register's index, 0..5. A 64-bit memory BAR also names the next
    /// register in `next`, which its own sizing must skip.
    usize index = 0;
    /// 1 when this BAR is a 64-bit memory BAR whose high half is the next
    /// register; the caller skips that register when probing the rest.
    bool is64 = false;
};

/// The command register's bits a driver enables once its bases are set.
inline constexpr u32 COMMAND_IO = 0x1;
inline constexpr u32 COMMAND_MEMORY = 0x2;
inline constexpr u32 COMMAND_MASTER = 0x4;

/// Reads the 32-bit BAR register |index| of function |index|--the caller's
/// window is the config space itself.
[[nodiscard]]
inline auto read_bar(Config &config, usize const index, usize const bar) -> u32 {
    return config.read32(index, 0x10 + bar * 4);
}

inline auto write_bar(Config &config, usize const index, usize const bar, u32 const value) -> void {
    config.write32(index, 0x10 + bar * 4, value);
}

/// Sizes the BAR |index| of function |index|: disables decode, stores all
/// ones, reads the size mask back, restores, and computes the size. Returns
/// the Bar and, for a 64-bit memory BAR, that the NEXT register is its high
/// half (the caller's probe skips it). NotFound for a vacant function.
[[nodiscard]]
inline auto size_bar(Config &config, usize const index, usize const bar)
    -> ustl::Result<Bar, ours::Status> {
    if (index >= pci::FUNCTIONS_PER_BUS || bar >= 6) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto const original_low = read_bar(config, index, bar);
    if ((original_low & 0x1) != 0) {
        // An I/O BAR: 2 low bits (space + reserved), the rest the size.
        write_bar(config, index, bar, 0xFFFF'FFFF);
        auto const mask = read_bar(config, index, bar) & 0xFFFF'FFFC;
        write_bar(config, index, bar, original_low);
        if (mask == 0) {
            return ustl::err(ours::Status::InvArgs);
        }
        auto size = static_cast<u64>(~mask + 1);
        return ustl::ok(Bar{.space = Space::Io, .size = size, .index = bar, .is64 = false});
    }
    auto const is64 = (original_low & 0x6) == 0x4;
    auto const high_reg = bar < 5 && is64 ? bar + 1 : bar;
    auto const original_high = is64 ? read_bar(config, index, high_reg) : 0;

    // Disable memory decode (the command register's bit 1) for the probe.
    auto const command = config.read32(index, 4);
    config.write32(index, 4, command & ~COMMAND_MEMORY);

    write_bar(config, index, bar, 0xFFFF'FFFF);
    auto const raw = read_bar(config, index, bar);
    auto const mask = raw & 0xFFFF'FFF0;
    // The high half's probe answer is read here, before its original word is
    // restored below -- reading it after the restore would see the old base,
    // not the sizing hardware's answer.
    auto high_mask = u32{0};
    if (is64 && bar < 5) {
        write_bar(config, index, high_reg, 0xFFFF'FFFF);
        high_mask = read_bar(config, index, high_reg);
    }
    write_bar(config, index, bar, original_low);
    if (is64 && bar < 5) {
        write_bar(config, index, high_reg, original_high);
    }
    config.write32(index, 4, command);

    // The size is the lowest set bit of the size mask -- the mask's
    // complement-plus-one, without the width traps the complement carries:
    // computed at 32 bits it caps a 64-bit BAR's decode at 4 GiB, and at
    // 64 bits it reads a hot-added device's all-ones high half (every bit
    // above the low half's is a size bit, so the high half of the mask is
    // all ones and its own complement contributes nothing) as two to the
    // 64th. The lowest set bit says the size whatever the width, whatever
    // the half.
    if (!(is64 && bar < 5)) {
        if (mask == 0) {
            return ustl::err(ours::Status::InvArgs);
        }
        return ustl::ok(Bar{.space = Space::Memory,
                            .size = static_cast<u64>(mask & (~mask + 1)),
                            .index = bar,
                            .is64 = is64});
    }
    auto const size_mask = static_cast<u64>(mask) | (static_cast<u64>(high_mask) << 32);
    if (size_mask == 0) {
        return ustl::err(ours::Status::InvArgs);
    }
    return ustl::ok(Bar{
        .space = Space::Memory, .size = size_mask & (~size_mask + 1), .index = bar, .is64 = is64});
}

/// Assigns the BAR |bar| of function |index| to |base|: the low half (and
/// the high half for a 64-bit memory BAR) written with the base's bits,
/// keeping the register's fixed low bits. The caller enables decode after
/// all BARs are set.
inline auto assign_bar(Config &config, usize const index, usize const bar, Bar const &shape,
                       u64 const base) -> void {
    auto const keep = shape.space == Space::Io ? 0x3u : 0xFu;
    auto const low = static_cast<u32>(base) | (read_bar(config, index, bar) & keep);
    write_bar(config, index, bar, low);
    if (shape.is64 && bar < 5) {
        write_bar(config, index, bar + 1, static_cast<u32>(base >> 32));
    }
}

/// Enables the function: memory and I/O decode, bus master. What a driver
/// calls after its BARs are assigned.
inline auto enable_function(Config &config, usize const index) -> void {
    auto const command = config.read32(index, 4);
    config.write32(index, 4, command | COMMAND_IO | COMMAND_MEMORY | COMMAND_MASTER);
}
} // namespace pci

#endif // #ifndef PCI_BAR_HPP
