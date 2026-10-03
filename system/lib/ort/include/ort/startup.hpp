/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ORT_STARTUP_HPP
#define ORT_STARTUP_HPP 1

#include <ort/process_args.hpp>

#include <ours/process_args.hpp>
#include <ours/types.hpp>

/// What a program was started with, as the runtime found it before main.
///
/// A program's entry point is the runtime's (_start, src/crt0.S): it reads
/// the processargs message on the bootstrap channel, keeps it, and calls
/// main(argc, argv) with the arguments in it. Everything else in the message
/// is reached through here.
namespace ort {
/// The exit code of a process the runtime could not start: its bootstrap
/// channel held no processargs message, or one that failed a check. main
/// never ran; the reason is in the debug log, one line starting "ort:".
///
/// Outside the range of int on purpose. main returns an int, which the
/// runtime widens to process_exit's i64, so no main can exit with this: a
/// parent reading it knows main never ran. (-1, the obvious choice, is the
/// most common thing a failing main returns.) The kernel's own codes,
/// TASK_RETCODE_* in <ours/types.hpp>, sit inside int's range and so do not
/// have that guarantee; this one needs it because it speaks for main.
inline constexpr auto EXIT_START_FAILED = i64{-2'147'483'648} - 1;

/// Takes the startup handle described by (|type|, |arg|): the caller owns it
/// from then on, and a second take of the same one answers HANDLE_INVALID,
/// as does asking for one the process was not given. Zircon
/// zx_take_startup_handle().
///
/// The runtime's own bookkeeping, not a system call: nothing reaches the
/// kernel here.
[[nodiscard]]
auto take_startup_handle(ours::HandleType type, u16 arg) -> ours::HandleValue;

/// The processargs message the process was started with: its environment
/// and namespace names, and the arguments main() already has. Empty for a
/// process started with no bootstrap channel.
[[nodiscard]]
auto process_args() -> ProcessArgs const &;

/// Where the vDSO is mapped in this process (%rsi at entry), for code that
/// has to find the vDSO's own image -- a symbolizer, a loader -- rather than
/// call it; calls into it are bound when the program is loaded.
[[nodiscard]]
auto vdso_base() -> ours::VirtAddr;
} // namespace ort

#endif // #ifndef ORT_STARTUP_HPP
