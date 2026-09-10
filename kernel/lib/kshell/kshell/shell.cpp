/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// A minimal serial-interactive shell over the gktl command registry. The
// registry (GKTL_COMMAND) declares commands; this is the missing front end:
// a bring-up console that reads COM1 lines and dispatches them, which is
// also how the sampling profiler gets driven in automated runs.
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <arch/x86/io.hpp>
#include <gktl/command.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ustl/fmt/format_to.hpp>

namespace kshell {
namespace {
    constexpr std::uint16_t COM1_PORT = 0x3F8;
    constexpr std::size_t LINE_MAX = 128;

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
        auto *thread = ours::task::Thread::spawn(
            "kshell", [](void *) -> i32 { return shell_main(); }, nullptr,
            ours::task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            log::warn("kshell: failed to spawn console thread");
        }
    }

    GKTL_INIT_HOOK(KShellInit, spawn, gktl::InitLevel::Platform)
} // namespace
} // namespace kshell
