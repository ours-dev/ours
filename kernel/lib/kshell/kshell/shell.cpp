/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// A minimal serial-interactive shell over the gktl command registry. The
// registry (GKTL_COMMAND) declares commands; this is the missing front end:
// a bring-up console that reads COM1 lines and dispatches them, which is
// also how the sampling profiler gets driven in automated runs.
#include <ours/boot/options.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <arch/x86/io.hpp>
#include <gktl/command.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/domain.hpp>
#include <logz4/log.hpp>
#include <ustl/fmt/format_to.hpp>

namespace kshell {
namespace {
    constexpr std::uint16_t COM1_PORT = 0x3F8;
    constexpr std::size_t LINE_MAX = 128;

    /// Every line this file logs carries `(kshell)` in place of the
    /// "kshell: " prefix the call sites used to spell out by hand, and
    /// `log level kshell debug` turns on the per-command trace in dispatch()
    /// without turning on anyone else's debug output.
    constexpr auto &KSHELL = logz4::Domain::named<"kshell">;

    auto serial_putc(char c) -> void {
        while ((arch::inb(COM1_PORT + 5) & 0x20) == 0) {
        }
        arch::outb(COM1_PORT, static_cast<std::uint8_t>(c));
    }

    auto serial_write(char const *s, std::size_t n) -> void {
        for (std::size_t i = 0; i < n; ++i) {
            serial_putc(s[i]);
        }
    }

    auto serial_read_ready() -> bool {
        return (arch::inb(COM1_PORT + 5) & 0x01) != 0;
    }

    auto serial_getc() -> char {
        return static_cast<char>(arch::inb(COM1_PORT));
    }

    auto dispatch(char *line) -> void {
        char *argv[16] = {};
        int argc = 0;
        char *cursor = line;
        while (*cursor != '\0' && argc < 16) {
            while (*cursor == ' ' || *cursor == '\t') {
                ++cursor;
            }
            if (*cursor == '\0') {
                break;
            }
            argv[argc] = cursor;
            ++argc;
            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
                ++cursor;
            }
            if (*cursor != '\0') {
                *cursor = '\0';
                ++cursor;
            }
        }
        if (argc != 0) {
            // Debug, not info: one line per command is noise at the prompt.
            // It earns its place when a command seems to do nothing, since
            // gktl::run_command() says nothing about a name it does not
            // know: this line shows what the shell parsed and handed over.
            KSHELL.debug("run '{}' with {} argument(s)", argv[0], argc - 1);
            gktl::run_command(argc, argv);
        }
    }

    auto shell_main() -> i32 {
        serial_write("\n", 1);
        char prompt[] = "kshell> ";
        serial_write(prompt, sizeof(prompt) - 1);

        char line[LINE_MAX] = {};
        std::size_t length = 0;
        for (;;) {
            if (!serial_read_ready()) {
                static_cast<void>(ours::task::Thread::Current::sleep_for(
                    ours::Milliseconds(10), ours::task::Interruptible::No));
                continue;
            }

            char const c = serial_getc();
            if (c == '\r' || c == '\n') {
                serial_write("\n", 1);
                line[length] = '\0';
                if (length != 0) {
                    dispatch(line);
                    length = 0;
                }
                serial_write(prompt, sizeof(prompt) - 1);
                continue;
            }
            if (c == 8 || c == 127) { // backspace / delete
                if (length != 0) {
                    --length;
                    serial_write("\b \b", 3);
                }
                continue;
            }
            if (length + 1 < LINE_MAX && c >= 32 && c < 127) {
                line[length] = c;
                ++length;
                serial_putc(c);
            }
        }
    }

    auto spawn() -> void {
        // `kernel.shell` is off unless the image asks for it: this thread runs
        // any registered command, in ring 0, for whoever can write to COM1.
        // Zircon makes the same check in the same place -- kernel_shell_init()
        // (kernel/lib/console/mod.rs) calls console_start() only when
        // gBootOptions->shell is set -- so a kernel that carries the shell
        // still has to be told to run it.
        //
        // Checked here, when the thread would start, rather than by leaving the
        // hook unregistered: registration is a linker-section record and cannot
        // depend on the command line, and not linking the shell at all is the
        // build's decision (kernel_shell_enabled), not the boot's.
        //
        // Said out loud rather than returning quietly: the last time this
        // console failed to appear (the thread was spawned and never resumed,
        // docs/changes/260910/kshell.md) nothing in the log said so, and
        // "no prompt" looked the same as "no shell".
        if (!ours::boot::options().shell()) {
            KSHELL.info("not started ({} is off)", ours::boot::BootOptions::SHELL_OPTION);
            return;
        }

        auto *thread = ours::task::Thread::spawn(
            "kshell", [](void *) -> i32 { return shell_main(); }, nullptr,
            ours::task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            KSHELL.warn("failed to spawn console thread");
            return;
        }

        // spawn() builds a thread; it does not start one. Without these the
        // console thread sits parked forever -- no prompt, no input, and no
        // complaint, because nothing failed. Every other caller in the tree
        // pairs spawn() with both (kernel/top/main.cpp:86, kernel/kernel/task/
        // dpc.cpp), and this one was the omission.
        //
        // detach() as well as resume(): nothing joins this thread and it never
        // returns, so leaving it joinable only keeps a corpse nobody collects.
        thread->detach();
        thread->resume();
        KSHELL.info("console on COM1");
    }

    GKTL_INIT_HOOK(KShellInit, spawn, gktl::InitLevel::Platform)
} // namespace
} // namespace kshell
