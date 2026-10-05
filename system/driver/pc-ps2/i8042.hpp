/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PC_PS2_I8042_HPP
#define PC_PS2_I8042_HPP 1

#include <ours/status.hpp>

#include <ustl/option.hpp>

/// The PC's keyboard controller, the i8042, as this driver drives it: the
/// keyboard on its first port, and nothing on its second, where a mouse
/// would be.
///
/// The controller is two I/O ports. The data port carries bytes from the
/// keyboard, and to it. The command and status port takes a command for the
/// controller itself when written, and gives the controller's status when
/// read. The process must have opened both with ioports_request before it
/// calls anything here: until then the first in or out is a
/// general-protection fault, which kills it.
namespace pc_ps2::i8042 {
inline constexpr auto DATA_PORT = u16{0x60};
inline constexpr auto COMMAND_PORT = u16{0x64};

/// Brings the controller and the keyboard to a known state, whatever the
/// firmware left them in, and has the keyboard send keys:
///   - both ports off, and what they had sent dropped;
///   - the controller's self-test and its first port's test;
///   - translation on, so the keyboard's keys reach the driver in scancode
///     set 1, whichever set the keyboard speaks;
///   - the first port on, with its interrupt, ISA 1; the second left off;
///   - the device on the first port asked who it is, and refused unless it
///     says it is a keyboard; then told to send its keys.
///
/// A step that fails says why on the debug console and is the answer:
/// TimeOut when the controller or the keyboard did not answer, BadState
/// when one answered wrongly, NotSupported when the device is no keyboard.
[[nodiscard]]
auto reset() -> ours::Status;

/// The next byte the keyboard sent, if the controller holds one. Reading it
/// lets the controller load the next: the driver takes bytes until there is
/// none.
[[nodiscard]]
auto read_byte() -> ustl::Option<u8>;
} // namespace pc_ps2::i8042

#endif // #ifndef PC_PS2_I8042_HPP
