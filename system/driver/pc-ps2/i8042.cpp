/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "i8042.hpp"

#include <ours/status.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>

#include <ort/debug.hpp>
#include <ustl/result.hpp>
#include <ustl/util/enum_bits.hpp>

namespace pc_ps2::i8042 {
namespace {
    using ours::Status;

    /// The bits of the status register this driver reads.
    enum class StatusBits : u8 {
        None = 0,
        /// A byte waits in the data port for the driver to read.
        OutputFull = 0_bit,
        /// The controller has not taken the last byte written to it yet: a
        /// byte written now would be lost.
        InputFull = 1_bit,
    };
    USTL_ENABLE_ENUM_BITMASK(StatusBits);

    /// The bits of the controller's configuration byte this driver sets or
    /// clears; it keeps the others as it read them.
    enum class Config : u8 {
        None = 0,
        /// An interrupt, on ISA 1, for each byte from the first port.
        Port1Interrupt = 0_bit,
        /// The same for the second port, on ISA 12.
        Port2Interrupt = 1_bit,
        /// The first port's clock is off: its device cannot send.
        Port1Disabled = 4_bit,
        /// The controller turns what the keyboard sends from scancode set 2
        /// into set 1.
        Translate = 6_bit,
    };
    USTL_ENABLE_ENUM_BITMASK(Config);

    /// A command, to the controller or to the keyboard: its code, how many
    /// bytes of parameter follow it on the data port, and how many bytes it
    /// answers with.
    struct Command {
        u8 code;
        u8 parameters;
        u8 replies;
    };

    // Commands to the controller, written to the command port.
    constexpr auto READ_CONFIG = Command{.code = 0x20, .parameters = 0, .replies = 1};
    constexpr auto WRITE_CONFIG = Command{.code = 0x60, .parameters = 1, .replies = 0};
    constexpr auto PORT2_DISABLE = Command{.code = 0xA7, .parameters = 0, .replies = 0};
    constexpr auto SELF_TEST = Command{.code = 0xAA, .parameters = 0, .replies = 1};
    constexpr auto PORT1_TEST = Command{.code = 0xAB, .parameters = 0, .replies = 1};
    constexpr auto PORT1_DISABLE = Command{.code = 0xAD, .parameters = 0, .replies = 0};

    // Commands to the keyboard, written to the data port: the controller
    // hands them to the device on its first port.
    constexpr auto IDENTIFY = Command{.code = 0xF2, .parameters = 0, .replies = 3};
    constexpr auto SCAN_ENABLE = Command{.code = 0xF4, .parameters = 0, .replies = 1};
    constexpr auto SCAN_DISABLE = Command{.code = 0xF5, .parameters = 0, .replies = 1};

    /// The controller's answer to its self-test when it passed.
    constexpr auto SELF_TEST_PASSED = u8{0x55};
    /// A port's answer to its test when it passed.
    constexpr auto PORT_TEST_PASSED = u8{0x00};
    /// The keyboard's answer to a command it took.
    constexpr auto ACK = u8{0xFA};
    /// The first byte of a keyboard's identity, after the ACK.
    constexpr auto KEYBOARD_ID = u8{0xAB};

    /// The most bytes the controller holds for the driver: a flush reads no
    /// more, in case a device keeps sending.
    constexpr auto MAX_BUFFERED = usize{32};

    /// How many times a wait reads the status port before it gives up. No
    /// program can read a clock or wait until a deadline yet (port_wait and
    /// object_wait_one take only 0 or INSTANT_INFINITE), so the bound is a
    /// count of reads. A read of a legacy port takes about a microsecond on
    /// hardware, which makes this about 100 ms there, longer than a
    /// controller or a keyboard takes to answer; under QEMU the answer is
    /// there at the first read.
    constexpr auto MAX_POLLS = u32{100'000};

    [[nodiscard]]
    auto inb(u16 const port) -> u8 {
        auto value = u8{0};
        asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
        return value;
    }

    auto outb(u16 const port, u8 const value) -> void {
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
    }

    [[nodiscard]]
    auto read_status() -> StatusBits {
        return static_cast<StatusBits>(inb(COMMAND_PORT));
    }

    /// Waits until the controller can take a byte; false if it never could.
    [[nodiscard]]
    auto wait_write() -> bool {
        for (auto poll = u32{0}; poll < MAX_POLLS; ++poll) {
            if (!(read_status() & StatusBits::InputFull)) {
                return true;
            }
        }
        return false;
    }

    /// Waits until a byte waits in the data port; false if none came.
    [[nodiscard]]
    auto wait_read() -> bool {
        for (auto poll = u32{0}; poll < MAX_POLLS; ++poll) {
            if (!!(read_status() & StatusBits::OutputFull)) {
                return true;
            }
        }
        return false;
    }

    /// Reads what the controller holds and drops it.
    auto flush() -> void {
        for (auto read = usize{0};
             read < MAX_BUFFERED && !!(read_status() & StatusBits::OutputFull); ++read) {
            // Sent before the ports went off: nobody asked for it.
            auto _ = inb(DATA_PORT);
        }
    }

    /// What a command answered: the bytes that came back, which may be
    /// fewer than it was due to send.
    struct Reply {
        std::array<u8, 3> bytes{};
        usize count = 0;
    };

    /// Writes |command| to |port|, the command port for a command to the
    /// controller and the data port for one to the keyboard, then its
    /// |parameters| to the data port, and reads its answer. A short answer is
    /// returned as it is, for the caller to judge; TimeOut when the
    /// controller would not take a byte, and InvArgs for parameters the
    /// command does not take.
    [[nodiscard]]
    auto send(Command const &command, u16 const port, std::span<u8 const> const parameters = {})
        -> ustl::Result<Reply, Status> {
        if (parameters.size() != command.parameters) {
            return ustl::err(Status::InvArgs);
        }
        if (!wait_write()) {
            return ustl::err(Status::TimeOut);
        }
        outb(port, command.code);
        for (auto const parameter: parameters) {
            if (!wait_write()) {
                return ustl::err(Status::TimeOut);
            }
            outb(DATA_PORT, parameter);
        }
        auto reply = Reply{};
        auto const due = std::min(usize{command.replies}, reply.bytes.size());
        while (reply.count < due && wait_read()) {
            reply.bytes[reply.count] = inb(DATA_PORT);
            reply.count += 1;
        }
        return ustl::ok(reply);
    }

    /// Says on the debug console that |what| failed with |status|.
    auto report(std::string_view const what, Status const status) -> void {
        auto _ = ort::debug_line({"pc-ps2: ", what, ": ", ours::to_string(status)});
    }

    /// Sends |command|, which answers nothing, to the controller.
    [[nodiscard]]
    auto run(std::string_view const what, Command const &command) -> Status {
        auto const sent = send(command, COMMAND_PORT);
        if (sent.is_err()) {
            report(what, sent.unwrap_err());
            return sent.unwrap_err();
        }
        return Status::Ok;
    }

    /// Sends |command| to |port| and checks that its one byte of answer is
    /// |expected|.
    [[nodiscard]]
    auto expect(std::string_view const what, Command const &command, u16 const port,
                u8 const expected) -> Status {
        auto const sent = send(command, port);
        if (sent.is_err()) {
            report(what, sent.unwrap_err());
            return sent.unwrap_err();
        }
        if (sent->count == 0) {
            report(what, Status::TimeOut);
            return Status::TimeOut;
        }
        if (sent->bytes[0] != expected) {
            char got[18];
            char due[18];
            auto _ =
                ort::debug_line({"pc-ps2: ", what, ": answered ", ort::hex(sent->bytes[0], got),
                                 " where ", ort::hex(expected, due), " was due"});
            return Status::BadState;
        }
        return Status::Ok;
    }

    [[nodiscard]]
    auto read_config() -> ustl::Result<Config, Status> {
        auto const sent = send(READ_CONFIG, COMMAND_PORT);
        if (sent.is_err()) {
            report("reading the configuration byte", sent.unwrap_err());
            return ustl::err(sent.unwrap_err());
        }
        if (sent->count == 0) {
            report("reading the configuration byte", Status::TimeOut);
            return ustl::err(Status::TimeOut);
        }
        return ustl::ok(static_cast<Config>(sent->bytes[0]));
    }

    [[nodiscard]]
    auto write_config(Config const config) -> Status {
        auto const byte = std::to_underlying(config);
        auto const sent = send(WRITE_CONFIG, COMMAND_PORT, std::span{&byte, 1});
        if (sent.is_err()) {
            report("writing the configuration byte", sent.unwrap_err());
            return sent.unwrap_err();
        }
        return Status::Ok;
    }

    /// Asks the device on the first port who it is, refuses anything but a
    /// keyboard, and has it send its keys.
    [[nodiscard]]
    auto identify_keyboard() -> Status {
        // Its keys off while it answers, so that a key pressed meanwhile is
        // not taken for part of the answer.
        if (auto const status =
                expect("disabling the keyboard's keys", SCAN_DISABLE, DATA_PORT, ACK);
            status != Status::Ok) {
            return status;
        }
        auto const identity = send(IDENTIFY, DATA_PORT);
        if (identity.is_err()) {
            report("identifying the keyboard", identity.unwrap_err());
            return identity.unwrap_err();
        }
        if (identity->count == 0 || identity->bytes[0] != ACK) {
            report("identifying the keyboard", Status::TimeOut);
            return Status::TimeOut;
        }
        // A keyboard answers its identity, two bytes, the first of them
        // KEYBOARD_ID; a mouse answers one byte, and the first keyboards
        // nothing at all.
        if (identity->count < 2 || identity->bytes[1] != KEYBOARD_ID) {
            auto _ = ort::debug_line("pc-ps2: the device on the first port is not a keyboard");
            return Status::NotSupported;
        }
        return expect("enabling the keyboard's keys", SCAN_ENABLE, DATA_PORT, ACK);
    }
} // namespace

auto reset() -> Status {
    // Both ports off, so that nothing a device sends from here on is taken
    // for an answer to the commands below; then what they had sent before
    // is dropped.
    if (auto const status = run("disabling the first port", PORT1_DISABLE); status != Status::Ok) {
        return status;
    }
    if (auto const status = run("disabling the second port", PORT2_DISABLE); status != Status::Ok) {
        return status;
    }
    flush();

    // The controller in its plainest mode while it tests itself and its
    // first port: no interrupt from either port, and no translation.
    auto const read = read_config();
    if (read.is_err()) {
        return read.unwrap_err();
    }
    auto config = read.unwrap();
    config &= ~(Config::Port1Interrupt | Config::Port2Interrupt | Config::Translate);
    if (auto const status = write_config(config); status != Status::Ok) {
        return status;
    }
    if (auto const status =
            expect("the controller's self-test", SELF_TEST, COMMAND_PORT, SELF_TEST_PASSED);
        status != Status::Ok) {
        return status;
    }
    if (auto const status =
            expect("the first port's test", PORT1_TEST, COMMAND_PORT, PORT_TEST_PASSED);
        status != Status::Ok) {
        return status;
    }

    // Translation on, and the first port on with its interrupt. The second
    // stays as PORT2_DISABLE left it, off and without its interrupt: no
    // driver here reads a mouse.
    config |= Config::Translate | Config::Port1Interrupt;
    config &= ~Config::Port1Disabled;
    if (auto const status = write_config(config); status != Status::Ok) {
        return status;
    }
    return identify_keyboard();
}

auto read_byte() -> ustl::Option<u8> {
    if (!(read_status() & StatusBits::OutputFull)) {
        return ustl::NONE;
    }
    return ustl::some(inb(DATA_PORT));
}
} // namespace pc_ps2::i8042
