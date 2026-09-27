/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// userboot -- the first program vmours runs in ring 3, and the one that
/// starts everything after it.
///
/// Zircon counterpart: //zircon/kernel/lib/userabi/userboot (main.rs). The
/// kernel always starts this program first (`kernel.select.userboot` picks
/// which build of it, and defaults to this one); what runs *next* is this
/// program's decision, made from `userboot.next=<name>` on the image's
/// command line. Keeping the two apart is the point: userboot is userboot,
/// init is init, and userboot starts init. An image that wants a different
/// system says so with `userboot.next`, not by putting another program in
/// userboot's place.
///
/// It runs on the runtime like every other user program (//system/lib/
/// runtime): _start reads the processargs message the kernel queued on the
/// bootstrap channel (make_bootstrap() in //kernel/lib/userabi/src/
/// userboot.cpp) and calls main(). That message carries the root job, the
/// BootFS, and -- as environment strings -- the image's `userboot.*`
/// options, which is how this program sees a command line it has no other
/// way to reach. Zircon's userboot reads its options out of the ZBI's
/// CMDLINE item instead (get_options() in main.rs); the kernel forwarding
/// just the `userboot.*` tokens spares this program a boot-container parser.

#include <string_view>

#include <ou/channel.hpp>
#include <ou/debug.hpp>
#include <ou/handle.hpp>
#include <ou/process.hpp>
#include <ou/str.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <runtime/main.hpp>
#include <runtime/startup.hpp>

namespace {
/// The option naming the program to start next, with its `=`: only the
/// `key=value` form means anything, since a bare `userboot.next` names no
/// program. Zircon's key of the same name (parse_option() in main.rs).
constexpr std::string_view NEXT_OPTION{"userboot.next="};

/// Prints "<what><status name>".
auto report(ou::Str const what, ours::Status const status) -> void {
    static_cast<void>(ou::debug_line({what, ou::Str::from_c_str(ours::to_string(status), 32)}));
}

auto to_str(std::string_view const text) -> ou::Str {
    return {text.data(), text.size()};
}

[[noreturn]]
auto halt() -> void {
    // `pause` is load-bearing, not decoration. C++'s forward-progress rule
    // makes a loop with no side effects undefined behaviour, and clang acts on
    // that licence: at -O2 it deletes `for (;;) {}` outright and lets control
    // fall off the end of the function. An `asm volatile` is a side effect the
    // optimiser must keep.
    //
    // userboot halts rather than exiting. Zircon's waits for the program it
    // started and then exits; here nothing yet reports a process's end
    // (TERMINATED on a process handle comes with the user-space loader,
    // design K23), and halting keeps the one thing that can still be
    // observed -- the timer interrupts the kernel goes on taking from user
    // space -- as evidence that ring 3 resumed after its last system call.
    for (;;) {
        __asm__ volatile("pause");
    }
}

/// The value of `userboot.next=` among the environment strings the kernel
/// passed, or empty when there is none.
///
/// The last one wins, as in Zircon's parse_cmdline(), which assigns each
/// option as it meets it: a later word on a command line overrides an
/// earlier one, which is what lets a bootloader append to an image's
/// defaults.
auto find_next() -> std::string_view {
    std::string_view next;
    for (auto const entry: runtime::process_args().environ()) {
        if (entry.starts_with(NEXT_OPTION)) {
            next = entry.substr(NEXT_OPTION.size());
        }
    }
    return next;
}

/// Starts |name| from the kernel package under |job|, handing it |job| and
/// |bootfs| as its JobDefault and VmoBootfs startup handles -- exactly what
/// the kernel handed this program, so the next program starts as it would
/// have had the kernel started it directly. argv is {|name|}.
///
/// The process is started before its processargs are written, the reverse
/// of the kernel's order (make_bootstrap()): |job| has to go into the
/// message, and channel_write takes every handle it sends out of this
/// process, so written first it would leave nothing to spawn under. The
/// other way round costs nothing -- the runtime waits for READABLE before
/// it reads -- and saves a handle_duplicate that would only leave userboot
/// holding a second root job handle it has no use for.
///
/// On success both handles have left this process. On failure to spawn they
/// are still here, and the caller decides what to do with them; a failure
/// after the spawn loses them with the message, and the new process, finding
/// PEER_CLOSED and no message, exits with runtime::EXIT_START_FAILED.
auto start_next(ours::HandleValue const job, ours::HandleValue const bootfs,
                std::string_view const name) -> ours::Status {
    using ours::Status;

    ours::HandleValue handles[2] = {job};
    u32 infos[2] = {ours::handle_info(ours::HandleType::JobDefault, 0)};
    u32 count = 1;
    if (bootfs != ours::HANDLE_INVALID) {
        handles[count] = bootfs;
        infos[count++] = ours::handle_info(ours::HandleType::VmoBootfs, 0);
    }

    // The header, two handle_infos and a name of at most 63 bytes and its
    // NUL -- the kernel keeps program names in 64 -- come to 108 bytes.
    u8 message[128];
    u32 length = 0;
    std::string_view const args[] = {name};
    if (auto const status =
            ours::encode_process_args(message, {infos, count}, args, {}, {}, &length);
        status != Status::Ok) {
        return status;
    }

    ours::HandleValue here = ours::HANDLE_INVALID;
    ours::HandleValue there = ours::HANDLE_INVALID;
    if (auto const status = ou::channel_create(&here, &there); status != Status::Ok) {
        return status;
    }
    if (auto const status = ou::process_spawn_boot(job, to_str(name), there);
        status != Status::Ok) {
        static_cast<void>(ou::handle_close(here));
        return status;
    }
    auto const written = ou::channel_write(here, message, length, handles, count);
    static_cast<void>(ou::handle_close(here));
    return written;
}
} // namespace

auto main(int, char **) -> int {
    auto const next = find_next();
    if (next.empty()) {
        // No program named: the proof-of-life image. Reaching this line at
        // all is what it proves -- the kernel built a process, mapped this
        // ELF and the vDSO into it, and the runtime read its processargs.
        static_cast<void>(ou::debug_line("userboot: hello from ring 3"));
        halt();
    }

    auto const job = runtime::take_startup_handle(ours::HandleType::JobDefault, 0);
    if (job == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("userboot: started without the root job"));
        halt();
    }
    // Absent is allowed: the kernel starts this program without a BootFS
    // when it could not build one, and the next program is the one that has
    // to cope with that, as init already does.
    auto const bootfs = runtime::take_startup_handle(ours::HandleType::VmoBootfs, 0);

    static_cast<void>(ou::debug_line({"userboot: starting ", to_str(next)}));
    if (auto const status = start_next(job, bootfs, next); status != ours::Status::Ok) {
        report("userboot: starting the next program = ", status);
        halt();
    }
    static_cast<void>(ou::debug_line({"userboot: started ", to_str(next)}));
    halt();
}
