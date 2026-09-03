#include <arch/x86/io.hpp>
#include <logz4/log.hpp>
#include <ours/phys/console.hpp>
#include <ours/phys/handoff.hpp>
#include <ustl/mem/object.hpp>

// Pre-logger panic path: logz4's emergency hook, overridden with strong
// linkage to poll COM1 directly — the console list may not exist yet (or
// the logger may not be installed) when an early panic fires, but the port
// GRUB/QEMU left behind still works.
auto logz4::emergency_write(char const *s, usize n) -> void {
    constexpr u16 COM1 = 0x3F8;
    auto const is_transmit_empty = [&]() { return arch::inb(COM1 + 5) & 0x20; };

    for (usize i = 0; i < n; ++i) {
        while (!is_transmit_empty()) {
        }
        arch::outb(COM1, static_cast<u8>(s[i]));
    }
}

namespace ours::phys {

struct SerialConsole: public Console {
    static constexpr u16 COM1 = 0x3F8;

    SerialConsole() : Console("Serial"), port_(COM1) {}

    auto init() -> void;
    auto write(char const *s, u32 n) -> void override;
    auto read(char *s, u32 n) -> u32 override;
    auto activate() -> void override final;

    u16 const port_;
};

auto SerialConsole::write(char const *s, u32 n) -> void {
    auto const is_transmit_empty = [&]() { return arch::inb(port_ + 5) & 0x20; };

    for (u32 i = 0; i < n; ++i) {
        while (!is_transmit_empty())
            ;
        arch::outb(port_, s[i]);
    }
}

auto SerialConsole::read(char *s, u32 n) -> u32 {
    auto const has_received = [&]() { return arch::inb(port_ + 5) & 1; };

    for (u32 i = 0; i < n; ++i) {
        while (!has_received())
            ;
        s[i] = arch::inb(port_);
    }

    return n;
}

auto SerialConsole::activate() -> void {
    arch::outb(port_ + 1, 0x00); // Disable all interrupts
    arch::outb(port_ + 3, 0x80); // Enable DLAB (set baud rate divisor)
    arch::outb(port_ + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    arch::outb(port_ + 1, 0x00); //                  (hi byte)
    arch::outb(port_ + 3, 0x03); // 8 bits, no parity, one stop bit
    arch::outb(port_ + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    arch::outb(port_ + 4, 0x0B); // IRQs enabled, RTS/DSR set
    arch::outb(port_ + 4, 0x1E); // Set in loopback mode, test the serial chip
    arch::outb(port_ + 0,
               0xAE); // Test serial chip (send byte 0xAE and check if serial returns same byte)

    if (arch::inb(port_ + 0) != 0xAE) {
        return;
    }

    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    arch::outb(port_ + 4, 0x0F);
}

namespace {
    // Auto-registers into Console's global list when
    // ccrt::init_global_static_objects() runs (see ours/phys/console.hpp).
    SerialConsole SERIAL_CONSOLE;
} // namespace

// Reads SERIAL_CONSOLE's own port rather than re-asserting COM1
// independently, so there is one source of truth for which port is live.
auto arch_uart_handoff() -> UartHandoff {
    return {.kind = UartKind::X86Pio16550, .base = SERIAL_CONSOLE.port_};
}

} // namespace ours::phys
