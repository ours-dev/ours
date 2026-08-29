#include <arch/x86/io.hpp>
#include <arch/x86/pic.hpp>

namespace arch {
    /// IO base address for master PIC
    static constexpr auto PIC1 = 0x20;
    static constexpr auto PIC1_CMD = PIC1;
    static constexpr auto PIC1_DATA = PIC1 + 1;
    /// IO base address for slave PIC
    static constexpr auto PIC2 = 0xA0;
    static constexpr auto PIC2_CMD = PIC2;
    static constexpr auto PIC2_DATA = PIC2 + 1;

    /// End-of-interrupt command code
    static constexpr auto PIC_EOI = 0x10;

    static constexpr auto ICW1_INIT = 0x11;

    /// Indicates that ICW4 will be present
    static constexpr auto ICW1_ICW4 = 0x01;

    /// 8086/88 (MCS-80/85) mode
    static constexpr auto ICW4_8086 = 0x01;

    auto Pic::wait_io() -> void {
        outb(0x80, 0);
    }

    auto Pic::disable() -> void {
        outb(PIC1_DATA, 0xFF);
        wait_io();
        outb(PIC2_DATA, 0xFF);
        wait_io();
    }

    /// init the PICs and remap them
    ///
    /// `pic1` is vector offset for master PIC
    /// `pic2` same for slave PIC
    auto Pic::remap(u8 pic1, u8 pic2) -> void {
        // Initialize PIC
        outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
        wait_io();
        outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
        wait_io();

        // send ICW2
        // ICW2: Master PIC vector offset
        outb(PIC1_DATA, pic1);
        wait_io();
        // ICW2: Slave PIC vector offset
        outb(PIC2_DATA, pic2);
        wait_io();

        // send ICW3
        // ICW3: tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
        outb(PIC1_DATA, 4);
        wait_io();
        // ICW3: tell Slave PIC its cascade identity (0000 0010)
        outb(PIC2_DATA, 2);
        wait_io();

        // send ICW4
        // ICW4: have the PICs use 8086 mode (and not 8080 mode)
        outb(PIC1_DATA, ICW4_8086);
        wait_io();
        outb(PIC2_DATA, ICW4_8086);
        wait_io();
    }

    auto Pic::issue(u8 irq) -> void {
        if (irq >= 8) {
            outb(PIC2_CMD, PIC_EOI);
        }
        outb(PIC1_CMD, PIC_EOI);
    }

    auto Pic::mask(u8 irq_line) -> void {
        u16 port;
        if (irq_line < 8) {
            port = PIC1_DATA;
        } else {
            port = PIC2_DATA;
            irq_line -= 8;
        }

        u8 value = static_cast<u8>(inb(port) | (1 << irq_line));
        outb(port, value);
        wait_io();
    }

    auto Pic::mask_all() -> void {
        outb(PIC1_DATA, 0xFF);
        wait_io();
        outb(PIC2_DATA, 0xFF);
        wait_io();
    }

    auto Pic::unmask(u8 irq_line) -> void {
        u16 port;
        if (irq_line < 8) {
            port = PIC1_DATA;
        } else {
            port = PIC2_DATA;
            irq_line -= 8;
        }

        u8 value = static_cast<u8>(inb(port) & ~(1 << irq_line));
        wait_io();
        outb(port, value);
        wait_io();
    }

    auto Pic::unmask_all() -> void {
        outb(PIC1_DATA, 0);
        wait_io();
        outb(PIC2_DATA, 0);
        wait_io();
    }

} // namespace arch
