/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The fake kernel's console and halt on riscv: the firmware's console,
/// reached through the SBI.

#include "arch.hpp"
#include <arch/halt.hpp>
#include <arch/interrupt.hpp>
#include <arch/riscv/sbi.hpp>

namespace ours::fake_kernel {
// The firmware owns the UART and waits for its transmitter itself, so there
// is no device here to set up or poll: one `ecall` per byte, the same call
// physload prints through.
auto write_char(char const character) -> void {
    ::arch::sbi::console_putchar(character);
}

[[noreturn]]
auto halt() -> void {
    ::arch::local_irq_disable();
    // wfi returns for any interrupt pending in sie, masked or not, so the
    // loop puts the hart back to sleep after a spurious wake.
    for (;;) {
        ::arch::halt();
    }
}

// physload prints through the same firmware call and records no UART for it:
// UartKind has no value for the SBI console, so the handoff's is None.  A
// handoff naming a UART here means physload found a device this image does
// not write to.
auto handoff_names_console(phys::UartHandoff const &uart) -> bool {
    return uart.kind == phys::UartKind::None;
}
} // namespace ours::fake_kernel
