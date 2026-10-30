/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/platform/power.hpp>
#include <ours/types.hpp>

#include <arch/halt.hpp>
#include <arch/spin_hint.hpp>
#include <arch/tick.hpp>
#include <arch/x86/io.hpp>

namespace ours {
namespace {
    /// The chipset's Reset Control Register, and the value that asks for a
    /// CPU and system reset. Intel's 100-series chipsets and AMD since
    /// Bulldozer define it this way, and so does the ICH9 that QEMU's q35
    /// models.
    constexpr auto RESET_CONTROL = u16{0xCF9};
    constexpr auto RESET_CPU_AND_SYSTEM = u8{0x06};

    /// The i8042 keyboard controller's command and status port, the status
    /// bit that says it cannot take a command yet, and the command that
    /// pulses the CPU's reset line: the fallback for when the chipset's reset
    /// register has not taken effect.
    constexpr auto I8042_PORT = u16{0x64};
    constexpr auto I8042_INPUT_FULL = u8{0x02};
    constexpr auto I8042_PULSE_RESET = u8{0xFE};

    /// How long each method gets to take effect before the next is tried:
    /// 100 M TSC ticks, between 20 and 100 ms on anything this runs on. A
    /// reset request is not instant; the next instructions run meanwhile.
    constexpr auto SETTLE_TICKS = usize{100'000'000};

    auto settle() -> void {
        auto const start = arch::Tick::get().count;
        while (arch::Tick::get().count - start < SETTLE_TICKS) {
            arch::spin_hint();
        }
    }

    /// The last resort: with an empty IDT, the breakpoint cannot be
    /// delivered, nor can the #GP or the #DF that follow, and the CPU shuts
    /// down -- which the chipset turns into a reset.
    [[noreturn]]
    auto triple_fault() -> void {
        struct [[gnu::packed]] {
            u16 limit;
            u64 base;
        } const empty{.limit = 0, .base = 0};
        asm volatile("lidt %0\n\tint3" ::"m"(empty));
        while (1) {
            arch::halt();
        }
    }
} // namespace

auto platform_reboot() -> void {
    arch::outb(RESET_CONTROL, RESET_CPU_AND_SYSTEM);
    settle();

    for (auto spins = usize{0}; spins < 100'000; ++spins) {
        if ((arch::inb(I8042_PORT) & I8042_INPUT_FULL) == 0) {
            break;
        }
        arch::spin_hint();
    }
    arch::outb(I8042_PORT, I8042_PULSE_RESET);
    settle();

    triple_fault();
}
} // namespace ours
