/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// init -- the first system service.
///
/// userboot starts it on the runtime (//system/lib/runtime), when the image
/// names it with `userboot.next=init`, with a processargs message holding
/// two startup handles: the root job and the BootFS VMO -- the ones the
/// kernel gave userboot, handed on unchanged. What it does with them today is the whole of the first
/// vertical slice (docs/design/260923/userspace-interface.md, section 5):
/// look at the BootFS, start the shell, say "ping" to it, sleep until the
/// shell answers, and print the answer. Every step goes through the vDSO;
/// nothing here executes a syscall instruction.
///
/// Between the ping and the wait it also starts the program the image's
/// `ours.autorun.boot` option names, if there is one -- Zircon's
/// `zircon.autorun.boot`, which console-launcher, the component that starts
/// Zircon's shell, runs in the same place. The option reaches init as an
/// environment string: the kernel forwards it to userboot, and userboot
/// hands on what is not its own.
///
/// The wait in the middle is the first in the system that really sleeps: the
/// answer can only come from another process, which has to be started,
/// scheduled -- on another CPU, as often as not -- and run to its reply. So
/// this program is also the test that a blocked system call comes back to
/// the right user stack (73bb1ca), that the wake is not lost (0f9e32b), and
/// that an allocation on an AP works (5b1ce2f).
///
/// It never returns from main: it is the process the system's other programs
/// are started from, and nothing yet would notice it gone.

#include <ours/process_args.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/types.hpp>

#include <string_view>

#include <bootfs/format.hpp>
#include <init/autorun.hpp>
#include <ou/channel.hpp>
#include <ou/debug.hpp>
#include <ou/handle.hpp>
#include <ou/object.hpp>
#include <ou/process.hpp>
#include <ou/str.hpp>
#include <ou/vmo.hpp>
#include <runtime/main.hpp>
#include <runtime/startup.hpp>

namespace {
/// Prints "<what><status name>".
auto report(ou::Str const what, ours::Status const status) -> void {
    static_cast<void>(ou::debug_line({what, ou::Str::from_c_str(ours::to_string(status), 32)}));
}

[[noreturn]]
auto halt() -> void {
    // `pause` is load-bearing, not decoration. C++'s forward-progress rule
    // makes a loop with no side effects undefined behaviour, and clang acts on
    // that licence: at -O2 it deletes `for (;;) {}` outright and lets control
    // fall off the end of the function. An `asm volatile` is a side effect the
    // optimiser must keep.
    //
    // init halts rather than exiting: it is the process the system's other
    // programs are started from, and nothing yet would notice it gone.
    for (;;) {
        __asm__ volatile("pause");
    }
}

constexpr ou::Str SHELL{"shell"};
constexpr ou::Str PING{"ping"};

auto to_str(std::string_view const text) -> ou::Str {
    return {text.data(), text.size()};
}

/// Reads the BootFS header through vmo_read and says how many files it
/// holds -- the first time a program looks at the BootFS the kernel built
/// for it (design K12), and the first vmo_read of a real VMO.
///
/// init keeps the handle: the shell gets no BootFS from it. Passing one on
/// would take handle_duplicate (K15), since init is about to need its own.
auto describe_bootfs(ours::HandleValue const bootfs) -> void {
    if (bootfs == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("init: started without a BootFS"));
        return;
    }
    bootfs::Header header{};
    if (auto const status = ou::vmo_read(bootfs, &header, 0, sizeof(header));
        status != ours::Status::Ok) {
        report("init: reading the BootFS header = ", status);
        return;
    }
    if (header.magic != bootfs::Header::MAGIC) {
        static_cast<void>(ou::debug_line("init: the BootFS header has the wrong magic"));
        return;
    }
    char count[10];
    static_cast<void>(
        ou::debug_line({"init: bootfs has ", ou::decimal(header.file_count, count), " entries"}));
}

/// Starts the autorun |command| under |job|, split by
/// init::AutorunCommand::parse(): its first `+`-separated word is the BootFS
/// name, and all of them, the name included, are its argv.
///
/// The program gets argv and nothing else: no job, since nothing it could be
/// asked to do yet starts processes, and no BootFS, which init keeps. What it
/// prints goes to the debug log like everyone else's.
auto start_autorun(ours::HandleValue const job, std::string_view const command) -> ours::Status {
    using ours::Status;

    init::AutorunCommand parsed;
    if (auto const status = init::AutorunCommand::parse(command, &parsed); status != Status::Ok) {
        return status;
    }

    // The header and the words. The words and their NULs take no more than
    // the option they came from, and the kernel relays options within
    // USER_OPTIONS_BYTES (<ours/process_args.hpp>), so a command always fits.
    constexpr usize MESSAGE_BYTES = sizeof(ours::ProcArgs) + ours::USER_OPTIONS_BYTES;
    u8 message[MESSAGE_BYTES];
    u32 length = 0;
    if (auto const status = ours::encode_process_args(message, {}, parsed.words(), {}, {}, &length);
        status != Status::Ok) {
        return status;
    }

    ours::HandleValue bootstrap = ours::HANDLE_INVALID;
    ours::HandleValue bootstrap_there = ours::HANDLE_INVALID;
    if (auto const status = ou::channel_create(&bootstrap, &bootstrap_there);
        status != Status::Ok) {
        return status;
    }
    auto const written = ou::channel_write(bootstrap, message, length);
    static_cast<void>(ou::handle_close(bootstrap));
    if (written != Status::Ok) {
        static_cast<void>(ou::handle_close(bootstrap_there));
        return written;
    }
    return ou::process_spawn_boot(job, to_str(parsed.name()), bootstrap_there);
}

/// Starts the shell under |job| with |peer| -- the far end of the channel
/// init talks to it on -- as its User0 startup handle, and argv {"shell"}.
///
/// The conversation cannot use the shell's bootstrap channel, which the
/// runtime closes as soon as it has read the processargs off it: init would
/// see PEER_CLOSED before the shell had said anything. The shell gets no job
/// either -- it needs none to answer a ping, and the dev image packages
/// nothing for it to start.
///
/// The message is written before the shell exists, as the kernel writes the
/// first process's. |peer| and the shell's bootstrap end leave this process
/// whatever the answer.
auto start_shell(ours::HandleValue const job, ours::HandleValue peer) -> ours::Status {
    using ours::Status;

    u8 message[128];
    u32 length = 0;
    u32 const infos[] = {ours::handle_info(ours::HandleType::User0, 0)};
    std::string_view const args[] = {{SHELL.data(), SHELL.size()}};
    if (auto const status = ours::encode_process_args(message, infos, args, {}, {}, &length);
        status != Status::Ok) {
        static_cast<void>(ou::handle_close(peer));
        return status;
    }

    ours::HandleValue bootstrap = ours::HANDLE_INVALID;
    ours::HandleValue bootstrap_there = ours::HANDLE_INVALID;
    if (auto const status = ou::channel_create(&bootstrap, &bootstrap_there);
        status != Status::Ok) {
        static_cast<void>(ou::handle_close(peer));
        return status;
    }
    auto const written = ou::channel_write(bootstrap, message, length, &peer, 1);
    static_cast<void>(ou::handle_close(bootstrap));
    if (written != Status::Ok) {
        static_cast<void>(ou::handle_close(bootstrap_there));
        return written;
    }
    return ou::process_spawn_boot(job, SHELL, bootstrap_there);
}
} // namespace

auto main(int, char **) -> int {
    using ours::Signals;
    using ours::Status;

    auto const job = runtime::take_startup_handle(ours::HandleType::JobDefault, 0);
    if (job == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("init: started without the root job"));
        halt();
    }
    static_cast<void>(ou::debug_line("init: started with the root job"));
    describe_bootfs(runtime::take_startup_handle(ours::HandleType::VmoBootfs, 0));

    // vmex, handed on by userboot: the capability to make a VMO executable
    // (design D3). init keeps it for its own launcher and hands it to nobody
    // (K23); until then holding it is all there is to check (K18).
    auto const vmex = runtime::take_startup_handle(ours::HandleType::SystemResource,
                                                   u16(ours::RSRC_SYSTEM_VMEX_BASE));
    if (vmex != ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("init: vmex resource ok"));
    } else {
        static_cast<void>(ou::debug_line("init: started without vmex"));
    }

    ours::HandleValue here = ours::HANDLE_INVALID;
    ours::HandleValue peer = ours::HANDLE_INVALID;
    if (auto const status = ou::channel_create(&here, &peer); status != Status::Ok) {
        report("init: channel_create = ", status);
        halt();
    }
    if (auto const status = start_shell(job, peer); status != Status::Ok) {
        report("init: spawning shell = ", status);
        halt();
    }
    static_cast<void>(ou::debug_line("init: spawned shell"));

    if (auto const status = ou::channel_write(here, PING.data(), static_cast<u32>(PING.size()));
        status != Status::Ok) {
        report("init: write(ping) = ", status);
        halt();
    }

    // The autorun program, now that the shell is up -- console-launcher
    // starts `zircon.autorun.boot` beside the console shell too
    // (LaunchAutorun() in its main.cc, just before RunSerialConsole()). Before the wait rather than after it: the shell may
    // never let go of its channel, and the command should not wait on that.
    // A failure is reported and nothing more; it is the command that failed,
    // not the system, and the shell is already running.
    // The last one on the command line wins (runtime::ProcessArgs::option()).
    if (auto const autorun = runtime::process_args().option(init::AUTORUN_OPTION);
        !autorun.empty()) {
        static_cast<void>(ou::debug_line({"init: autorun ", to_str(autorun)}));
        if (auto const status = start_autorun(job, autorun); status != Status::Ok) {
            report("init: starting the autorun command = ", status);
        }
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

    // And then until the shell lets go of its end: it closes it as it checks
    // that closing a handle works, or its process dying closes it. Either way
    // this is the shell being done with init, not necessarily the shell being
    // gone -- that sign is TERMINATED on a process handle, which comes with
    // the user-space loader (design K23).
    status = ou::object_wait_one(here, Signals::PeerClosed, ours::INSTANT_INFINITE, &observed);
    if (status != Status::Ok) {
        report("init: waiting for the shell to finish = ", status);
        halt();
    }
    static_cast<void>(ou::debug_line("init: shell closed its channel"));

    halt();
}
