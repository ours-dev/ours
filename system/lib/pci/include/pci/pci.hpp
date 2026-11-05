/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PCI_PCI_HPP
#define PCI_PCI_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <array>
#include <span>
#include <string_view>
#include <utility>

#include <ustl/result.hpp>

/// The PCI configuration-space reader and the bus-0 enumerator, over an
/// ECAM window the caller maps (the route service names its base; 4 KiB per
/// function page). This is the shared walk the drivers each hand-rolled --
/// and the BAR probe and assignment a hot-plugged device needs: firmware
/// assigns BARs at boot only, so a device that appears later is lit by
/// writing its bars here, from the sizing this library computes.
///
/// Bytes only, no system call: it reads whatever window the caller maps,
/// and the tests feed it an array -- a fake ECAM in host memory.
namespace pci {

/// The ECAM stride: one 4 KiB function page per bus-device-function, the
/// geometry the route service's window is laid out with.
inline constexpr auto FUNCTION_STRIDE = usize{4096};
inline constexpr auto DEVICES_PER_BUS = usize{32};
inline constexpr auto FUNCTIONS_PER_DEVICE = usize{8};

/// A configuration space: the ECAM window's bytes for one bus, and the
/// reads and writes a driver needs on its functions. All access is through
/// the window; nothing here talks to a device.
///
/// The optional probe-mask table is the sizing hardware's answer sheet: one
/// word per config word, what the device reads back when all ones are
/// stored to that register (the sizing write). Real hardware answers the
/// mask; the fake in a host test seeds the table, and the same code path
/// -- write all ones, read the mask -- runs in both.
class Config final {
  public:
    /// Over |bytes|, the bus's ECAM pages (32 functions x 4 KiB). The
    /// |probe_masks| view, one word per config word (offset/4), is the
    /// sizing hardware's answer sheet; empty for a window whose reads
    /// return what was stored.
    Config(std::span<u8> bytes, std::span<u32 const> const probe_masks = {})
        : m_bytes(bytes), m_probe_masks(probe_masks) {}

    /// The 16-bit register at |offset| of function |index| (0..31).
    [[nodiscard]]
    auto read16(usize const index, usize const offset) const -> u16 {
        auto const at = index * FUNCTION_STRIDE + offset;
        return static_cast<u16>(static_cast<u16>(m_bytes[at]) |
                                static_cast<u16>(static_cast<u16>(m_bytes[at + 1]) << 8));
    }

    /// The 32-bit register at |offset| of function |index|. When the stored
    /// word is all ones and the function's own probe-mask entry names this
    /// word, the mask is what reads back -- the sizing hardware's answer,
    /// keyed per function like the table's writer seeds it.
    [[nodiscard]]
    auto read32(usize const index, usize const offset) const -> u32 {
        u32 value = 0;
        for (auto i = usize{0}; i < 4; ++i) {
            value |= static_cast<u32>(m_bytes[index * FUNCTION_STRIDE + offset + i]) << (8 * i);
        }
        auto const word = offset / 4 + index * (FUNCTION_STRIDE / 4);
        if (value == 0xFFFF'FFFF && word < m_probe_masks.size()) {
            return m_probe_masks[word];
        }
        return value;
    }

    /// Writes the 32-bit register at |offset| of function |index|.
    auto write32(usize index, usize offset, u32 value) -> void {
        for (auto i = usize{0}; i < 4; ++i) {
            m_bytes[index * FUNCTION_STRIDE + offset + i] =
                static_cast<u8>((value >> (8 * i)) & 0xFF);
        }
    }

  private:
    /// The window, mutable: the BAR probe writes all ones through it.
    std::span<u8> m_bytes;
    std::span<u32 const> m_probe_masks;
};

/// One in-use function, as the walk reports it: the ids a driver matches
/// and the raw BAR registers it probes.
struct Function {
    usize index = 0;
    u16 vendor = 0;
    u16 device = 0;
    /// Class code as the config space packs it: base<<16 | sub<<8 | iface.
    u32 class_code = 0;
    /// The six raw BAR registers, read as they lie.
    std::array<u32, 6> bars{};
};

/// Walks bus 0's 32 functions, visiting each in-use one (vendor register
/// reads 0xFFFF for the vacant slots). |visit| answers false to stop early.
template <typename Visit>
auto walk_bus0(Config const &config, Visit &&visit) -> void {
    for (auto index = usize{0}; index < DEVICES_PER_BUS; ++index) {
        if (config.read16(index, 0) == 0xFFFF) {
            continue;
        }
        auto fn = Function{.index = index,
                           .vendor = config.read16(index, 0),
                           .device = config.read16(index, 2),
                           .class_code = config.read32(index, 8) >> 8};
        for (auto bar = usize{0}; bar < 6; ++bar) {
            fn.bars[bar] = config.read32(index, 0x10 + bar * 4);
        }
        if (!visit(fn)) {
            return;
        }
    }
}
} // namespace pci

#endif // #ifndef PCI_PCI_HPP
