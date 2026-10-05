/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/platform/ioport.hpp>

#include <array>

namespace ours::platform {
namespace {
    /// The PC's ports the kernel drives, each beside the code that drives it.
    ///
    /// The i8042's data port, 0x60, and its command and status port, 0x64,
    /// are not among them: the PS/2 keyboard driver, a user program
    /// (system/driver/pc-ps2), sets the controller up and reads every
    /// scancode. platform_reboot() still writes one command to 0x64
    /// (power.cpp), its fallback when the chipset's reset register has not
    /// reset the machine; that is the last thing a machine going down does,
    /// and takes nothing from the driver. Granting 0x64 grants that command
    /// with it: the controller that reads the keyboard is also wired to the
    /// CPU's reset line, so a program that holds the port can reset the
    /// machine, which no program can do through 0xCF9 below.
    constexpr auto KERNEL_IOPORTS = std::array{
        // The master PIC's command and data ports. platform_init_apic()
        // (interrupt.cpp) masks every line on both PICs before it hands
        // interrupts to the IO-APIC, and counts on them staying masked: the
        // IO-APIC is to be the one way a device's interrupt comes in.
        IoPortRange{.base = 0x20, .count = 2},
        // The PIT's three counters and its mode register: a clock-event device
        // of the kernel's (kernel/driver/clock/i8253.cpp), which drives the
        // timer line until the HPET takes the line over.
        IoPortRange{.base = 0x40, .count = 4},
        // The slave PIC, masked with the master.
        IoPortRange{.base = 0xA0, .count = 2},
        // COM1, the kernel's console: its eight registers, which the log
        // (logz4), the kernel shell (kshell) and debug_read
        // (debug_console.cpp) all drive.
        IoPortRange{.base = 0x3F8, .count = 8},
        // The chipset's reset control register, through which
        // platform_reboot() resets the machine (power.cpp). It lies inside
        // the PCI configuration ports 0xCF8-0xCFF, so a driver that uses
        // those will need this entry looked at again.
        IoPortRange{.base = 0xCF9, .count = 1},
    };

    /// What kernel_ioports() promises its callers: sorted by base, no range
    /// empty, none past the 16-bit port space, and no two overlapping.
    consteval auto sorted_and_disjoint() -> bool {
        auto end = u32{0};
        for (auto const &range: KERNEL_IOPORTS) {
            auto const next = u32{range.base} + range.count;
            if (range.count == 0 || range.base < end || next > u32{1} << 16) {
                return false;
            }
            end = next;
        }
        return true;
    }
    static_assert(sorted_and_disjoint());
} // namespace

auto kernel_ioports() -> ustl::views::Span<IoPortRange const> {
    return KERNEL_IOPORTS;
}
} // namespace ours::platform
