/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `crash <how>`: take the machine down on purpose, so that the panic path can
// be tested the way it is used. Zircon's `crash` debug command
// (kernel/debugcommands/debugcommands.cc) is the model, down to the name.
//
//   crash panic    call ours::panic() with a fixed message
//
// The panic path cannot be exercised any other way: nothing panics in a
// healthy boot, and what it prints, and in what order, is the whole of what
// there is to check. scripts/kshell_run.py knows that a `crash` command ends
// the machine, and checks the report that follows it.
#include <ours/panic.hpp>

#include <string_view>

#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace {
/// What `crash panic` panics with. Fixed, so that a check reading the serial
/// log knows which line to look for.
constexpr auto PANIC_MESSAGE = std::string_view("crash: deliberate panic from the kernel shell");

auto crash_command(int argc, char **argv) -> int {
    if (argc == 2 && std::string_view(argv[1]) == "panic") {
        ours::panic(PANIC_MESSAGE);
    }
    log::warn("usage: crash panic");
    return 1;
}
} // namespace

GKTL_COMMAND(Crash, "crash", crash_command, "crash panic: panic on purpose, to test the panic path")
