/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// init -- the first system service.
///
/// The kernel starts it with one handle, the root job, and nothing else. What
/// it does with that today is the whole of the first vertical slice
/// (docs/design/260923/userspace-interface.md, section 5): make a channel,
/// start the shell with one end of it, say "ping", sleep until the shell
/// answers, and print the answer. Every step goes through the vDSO; nothing
/// here executes a syscall instruction.
///
/// The wait in the middle is the first in the system that really sleeps: the
/// answer can only come from another process, which has to be started,
/// scheduled -- on another CPU, as often as not -- and run to its reply. So
/// this program is also the test that a blocked system call comes back to
/// the right user stack (73bb1ca), that the wake is not lost (0f9e32b), and
/// that an allocation on an AP works (5b1ce2f).
///
/// There is no exit system call yet, so it ends by spinning, as every program
/// here does.

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

#include <ou/channel.hpp>
#include <ou/debug.hpp>
#include <ou/object.hpp>
#include <ou/process.hpp>
#include <ou/str.hpp>

namespace {
/// Prints "<what><status name>".
auto report(ou::Str const what, ours::Status const status) -> void {
    static_cast<void>(ou::debug_line({what, ou::Str::from_c_str(ours::to_string(status), 32)}));
}

[[noreturn]]
auto halt() -> void {
    // See examples/shell/shell.cpp for why `pause` is load-bearing: C++
    // licenses the optimiser to delete a loop with no side effects.
    for (;;) {
        __asm__ volatile("pause");
    }
}

constexpr ou::Str SHELL{"shell"};
constexpr ou::Str PING{"ping"};
} // namespace

/// |job| is the root job (%rdi); the vDSO base (%rsi) is not needed.
extern "C" [[noreturn]]
auto _start(ours::HandleValue const job, [[maybe_unused]] ours::VirtAddr const vdso_base) -> void {
    using ours::Signals;
    using ours::Status;

    static_cast<void>(ou::debug_line("init: started with the root job"));

    ours::HandleValue here = ours::HANDLE_INVALID;
    ours::HandleValue there = ours::HANDLE_INVALID;
    if (auto const status = ou::channel_create(&here, &there); status != Status::Ok) {
        report("init: channel_create = ", status);
        halt();
    }

    // |there| leaves this process here, whether or not the spawn works.
    if (auto const status = ou::process_spawn_boot(job, SHELL, there); status != Status::Ok) {
        report("init: spawning shell = ", status);
        halt();
    }
    static_cast<void>(ou::debug_line("init: spawned shell"));

    if (auto const status = ou::channel_write(here, PING.data(), static_cast<u32>(PING.size()));
        status != Status::Ok) {
        report("init: write(ping) = ", status);
        halt();
    }

    // Asleep until the shell writes back, or its end of the channel goes.
    auto observed = Signals::None;
    auto status = ou::object_wait_one(here, Signals::Readable | Signals::PeerClosed,
                                      ours::INSTANT_INFINITE, &observed);
    if (status != Status::Ok || !(observed & Signals::Readable)) {
        report("init: waiting for the shell = ", status);
        static_cast<void>(ou::debug_line("init: the shell closed the channel without answering"));
        halt();
    }

    char buffer[32] = {};
    u32 actual = 0;
    status = ou::channel_read(here, buffer, sizeof(buffer), &actual);
    if (status != Status::Ok) {
        report("init: read = ", status);
        halt();
    }
    static_cast<void>(ou::debug_line({"init: shell says \"", ou::Str(buffer, actual), "\""}));

    halt();
}
