/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/phys/console.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>

#include <arch/riscv/sbi.hpp>
#include <logz4/log.hpp>

namespace {
/// The firmware's console takes one byte per call, and needs no setup: it
/// works from the first instruction S-mode runs.
auto sbi_write(char const *const s, usize const n) -> void {
    for (usize i = 0; i < n; ++i) {
        arch::sbi::console_putchar(s[i]);
    }
}
} // namespace

/// Pre-logger panic path: logz4's emergency hook, defined with strong linkage
/// over its default, which drops the text. A panic before the logger is
/// installed still reaches the firmware's console.
auto logz4::emergency_write(char const *const s, usize const n) -> void {
    sbi_write(s, n);
}

namespace ours::phys {
namespace {
    /// phys's console on riscv: the legacy SBI putchar.
    class SbiConsole final: public Console {
      public:
        SbiConsole() : Console("Sbi") {}

        auto write(char const *const s, u32 const n) -> void override {
            sbi_write(s, n);
        }

        /// Input is not wired: nothing in phys reads a console.
        auto read(char *const, u32 const) -> u32 override {
            return 0;
        }
    };

    // Auto-registers into Console's global list when
    // ccrt::init_global_static_objects() runs (see ours/phys/console.hpp).
    SbiConsole SBI_CONSOLE;
} // namespace

/// No UART for the kernel to take over. phys never found or configured one;
/// it printed through the firmware, which the kernel can call just the same.
auto arch_uart_handoff() -> UartHandoff {
    return {.kind = UartKind::None};
}

} // namespace ours::phys
