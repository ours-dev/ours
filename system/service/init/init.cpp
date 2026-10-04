/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// init -- the first system service.
///
/// userboot starts it on the runtime (//system/lib/ort), when the image
/// names it with `userboot.next=bin/init`, with a processargs message holding
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
/// Every program init starts, the shell and the autorun command, is started
/// by the user-space launcher (launch::launch_from_bootfs()); once user space
/// is running, the kernel loads no program for it. The launcher hands init
/// the process handle: once the shell has let go of init's channel, init
/// waits for it to terminate and says what it ended with (object_get_info).
/// Without what a launcher needs -- the BootFS, vmex, the vDSO's VMO and
/// init's own root region -- init starts nothing and halts.
///
/// It never returns from main: it is the process the system's other programs
/// are started from, and nothing yet would notice it gone.

#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <span>
#include <string_view>

#include "programs.hpp"
#include <bootfs/format.hpp>
#include <elfload/diagnostics.hpp>
#include <init/autorun.hpp>
#include <launch/bootfs.hpp>
#include <launch/launch.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>

namespace {
/// Prints "<what><status name>".
auto report(std::string_view const what, ours::Status const status) -> void {
    auto _ = ort::debug_line({what, std::string_view{ours::to_string(status)}});
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

constexpr std::string_view PING{"ping"};

auto to_str(std::string_view const text) -> std::string_view {
    return {text.data(), text.size()};
}

/// Reads the BootFS header through vmo_read and says how many files it
/// holds -- the first time a program looks at the BootFS the kernel built
/// for it (design K12), and the first vmo_read of a real VMO.
///
/// init keeps the handle: the shell gets no BootFS from it. Passing one on
/// would take handle_duplicate (K15), since init is about to need its own.
auto describe_bootfs(ours::Uandle const bootfs) -> void {
    if (bootfs == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: started without a BootFS");
        return;
    }
    bootfs::Header header{};
    if (auto const status = ours::vmo_read(bootfs, &header, 0, sizeof(header));
        status != ours::Status::Ok) {
        report("init: reading the BootFS header = ", status);
        return;
    }
    if (header.magic != bootfs::Header::MAGIC) {
        auto _ = ort::debug_line("init: the BootFS header has the wrong magic");
        return;
    }
    char count[10];
    auto _ =
        ort::debug_line({"init: bootfs has ", ort::decimal(header.file_count, count), " entries"});
}

/// The launcher's failures, on the debug console. Only the errors: the
/// shell's printer also shows every import bound, which init's log does not
/// need.
auto print_launch_error(void *, elfload::Report const &report) -> void {
    if (report.severity != elfload::Severity::Error) {
        return;
    }
    std::string_view separator;
    std::string_view name;
    if (!report.name.empty()) {
        separator = ": ";
        name = {report.name.data(), report.name.size()};
    }
    auto _ = ort::debug_line(
        {"init: launch failed: ", {report.what.data(), report.what.size()}, separator, name});
}

/// Starts |name| with |there| as its bootstrap end, then writes |message| on
/// |here| with the child's root VMA (VmaRoot) after |handles| -- the order a
/// launcher keeps: the process first, then the processargs, since the root
/// VMA only exists once the process does. The message's handle_infos must
/// list |handles| and then VmaRoot. Closes |here|; |there| and |handles|
/// leave this process whatever the answer.
///
/// The program is started by |launcher|, and its process handle comes back in
/// |process|, which the caller then owns.
auto start_then_write(launch::BootfsLauncher const &launcher, ours::Uandle const job,
                      std::string_view const name, ours::Uandle here, ours::Uandle there,
                      std::span<u8 const> const message,
                      std::span<ours::Uandle const> const handles, ours::Uandle *const process)
    -> ours::Status {
    using ours::Status;

    ours::Uandle all[8] = {};
    ours::Uandle root_vma = ours::Uandle::Invalid;
    auto spawned = Status::Ok;
    auto const started = launch::launch_from_bootfs(
        launcher, job, name, there, {.report = print_launch_error, .context = nullptr});
    if (started.is_err()) {
        spawned = started.unwrap_err();
    } else {
        *process = started.unwrap().process;
        root_vma = started.unwrap().root_vma;
    }
    if (spawned != Status::Ok) {
        for (auto const handle: handles) {
            auto _ = ours::handle_close(handle);
        }
        auto _ = ours::handle_close(here);
        return spawned;
    }
    u32 count = 0;
    for (auto const handle: handles) {
        all[count++] = handle;
    }
    all[count++] = root_vma;
    auto const written = ours::channel_write(here, ours::ChannelWriteOptions::None, message.data(),
                                             static_cast<u32>(message.size()), all, count);
    auto _ = ours::handle_close(here);
    return written;
}

/// Starts the autorun |command| under |job|, split by
/// init::AutorunCommand::parse(): its first `+`-separated word is the BootFS
/// name, and all of them, the name included, are its argv.
///
/// The program gets argv and nothing else: no job, since nothing it could be
/// asked to do yet starts processes, and no BootFS, which init keeps. What it
/// prints goes to the debug log like everyone else's. init does not wait for
/// it, so the process handle the launcher hands back is closed at once:
/// closing it does not stop a process that has started.
auto start_autorun(launch::BootfsLauncher const &launcher, ours::Uandle const job,
                   std::string_view const command) -> ours::Status {
    using ours::Status;

    init::AutorunCommand parsed;
    if (auto const status = init::AutorunCommand::parse(command, &parsed); status != Status::Ok) {
        return status;
    }

    // The header and the words. The words and their NULs take no more than
    // the option they came from, and the kernel relays options within
    // USER_OPTIONS_BYTES (<ours/process_args.hpp>), so a command always fits.
    // One handle, the program's root VMA, known only once it is spawned.
    constexpr usize MESSAGE_BYTES = sizeof(ours::ProcArgs) + sizeof(u32) + ours::USER_OPTIONS_BYTES;
    u8 message[MESSAGE_BYTES];
    u32 length = 0;
    u32 const infos[] = {ours::handle_info(ours::HandleType::VmaRoot, 0)};
    if (auto const status =
            ours::encode_process_args(message, infos, parsed.words(), {}, {}, &length);
        status != Status::Ok) {
        return status;
    }

    ours::Uandle bootstrap = ours::Uandle::Invalid;
    ours::Uandle bootstrap_there = ours::Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &bootstrap, &bootstrap_there);
        status != Status::Ok) {
        return status;
    }
    auto process = ours::Uandle::Invalid;
    auto const status = start_then_write(launcher, job, to_str(parsed.name()), bootstrap,
                                         bootstrap_there, {message, length}, {}, &process);
    if (process != ours::Uandle::Invalid) {
        auto _ = ours::handle_close(process);
    }
    return status;
}

/// Starts the shell under |job| with |peer| as its User0 startup handle,
/// duplicated launcher handles, and the System Debug resource for COM1.
///
/// The liveness channel is separate from the bootstrap channel, which the
/// runtime closes as soon as it has read processargs. The shell answers init's
/// ping on the liveness channel, then keeps running its command loop.
///
/// The message is written after the shell exists (start_then_write()), since
/// its root VMA goes in it. The duplicated handles and |peer| leave init with
/// the processargs message; init retains its original launcher handles.
///
/// Started by |launcher|, which puts the shell's process handle in |process|.
auto start_shell(launch::BootfsLauncher const &launcher, ours::Uandle const job,
                 ours::Uandle const bootfs, ours::Uandle const vmex, ours::Uandle const vdso,
                 ours::Uandle debug, ours::Uandle peer, ours::Uandle *const process)
    -> ours::Status {
    using ours::Status;

    if (debug == ours::Uandle::Invalid) {
        auto _ = ours::handle_close(peer);
        return Status::BadHandle;
    }

    ours::Uandle handed[6] = {};
    u32 infos[7] = {};
    u32 count = 0;
    handed[count] = peer;
    infos[count++] = ours::handle_info(ours::HandleType::User0, 0);

    auto const append_duplicate = [&](ours::Uandle const original, ours::HandleType const type,
                                      u16 const arg) -> Status {
        if (original == ours::Uandle::Invalid) {
            return Status::Ok;
        }
        auto duplicate = ours::Uandle::Invalid;
        if (auto const status =
                ours::handle_duplicate(original, ours::Rights::SameRights, &duplicate);
            status != Status::Ok) {
            return status;
        }
        handed[count] = duplicate;
        infos[count++] = ours::handle_info(type, arg);
        return Status::Ok;
    };

    if (auto const status = append_duplicate(job, ours::HandleType::JobDefault, 0);
        status != Status::Ok) {
        for (u32 index = 0; index < count; ++index) {
            auto _ = ours::handle_close(handed[index]);
        }
        return status;
    }
    if (auto const status = append_duplicate(bootfs, ours::HandleType::VmoBootfs, 0);
        status != Status::Ok) {
        for (u32 index = 0; index < count; ++index) {
            auto _ = ours::handle_close(handed[index]);
        }
        return status;
    }
    if (auto const status = append_duplicate(vmex, ours::HandleType::SystemResource,
                                             u16(ours::RSRC_SYSTEM_VMEX_BASE));
        status != Status::Ok) {
        for (u32 index = 0; index < count; ++index) {
            auto _ = ours::handle_close(handed[index]);
        }
        return status;
    }
    if (auto const status = append_duplicate(vdso, ours::HandleType::VmoVdso, 0);
        status != Status::Ok) {
        for (u32 index = 0; index < count; ++index) {
            auto _ = ours::handle_close(handed[index]);
        }
        return status;
    }
    handed[count] = debug;
    infos[count++] =
        ours::handle_info(ours::HandleType::SystemResource, u16(ours::RSRC_SYSTEM_DEBUG_BASE));
    infos[count] = ours::handle_info(ours::HandleType::VmaRoot, 0);

    u8 message[256];
    u32 length = 0;
    std::string_view const args[] = {user_programs::SHELL};
    if (auto const status =
            ours::encode_process_args(message, {infos, count + 1}, args, {}, {}, &length);
        status != Status::Ok) {
        for (u32 index = 0; index < count; ++index) {
            auto _ = ours::handle_close(handed[index]);
        }
        return status;
    }

    ours::Uandle bootstrap = ours::Uandle::Invalid;
    ours::Uandle bootstrap_there = ours::Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &bootstrap, &bootstrap_there);
        status != Status::Ok) {
        for (u32 index = 0; index < count; ++index) {
            auto _ = ours::handle_close(handed[index]);
        }
        return status;
    }
    return start_then_write(launcher, job, user_programs::SHELL, bootstrap, bootstrap_there,
                            {message, length}, {handed, count}, process);
}

/// The code |process| ended with, said on the console: TERMINATED first,
/// since until then a kill may still change it, then object_get_info.
auto report_exit(ours::Uandle const process) -> void {
    using ours::Status;

    if (auto const status = ours::object_wait_one(process, ours::Signals::Terminated,
                                                  ours::INSTANT_INFINITE, nullptr);
        status != Status::Ok) {
        report("init: waiting for the shell to end = ", status);
        return;
    }
    ours::InfoProcess info{};
    if (auto const status = ours::object_get_info(process, ours::InfoTopic::Process, &info,
                                                  sizeof(info), nullptr, nullptr);
        status != Status::Ok) {
        report("init: object_get_info(the shell) = ", status);
        return;
    }
    char code[21];
    auto _ =
        ort::debug_line({"init: shell exited with ", ort::signed_decimal(info.return_code, code)});
}
} // namespace

auto main(int, char **) -> int {
    using ours::Signals;
    using ours::Status;

    auto const job = ort::take_startup_handle(ours::HandleType::JobDefault, 0);
    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: started without the root job");
        halt();
    }
    auto _ = ort::debug_line("init: started with the root job");
    auto const bootfs = ort::take_startup_handle(ours::HandleType::VmoBootfs, 0);
    describe_bootfs(bootfs);

    // vmex, handed on by userboot: the capability to make a VMO executable
    // (design D3). init keeps it for its own launcher and hands it to nobody
    // (K23); until then holding it is all there is to check (K18).
    auto const vmex = ort::take_startup_handle(ours::HandleType::SystemResource,
                                               u16(ours::RSRC_SYSTEM_VMEX_BASE));
    if (vmex != ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: vmex resource ok");
    } else {
        auto _ = ort::debug_line("init: started without vmex");
    }

    // The vDSO's VMO, also handed on by userboot: init maps it into the
    // processes it starts once its launcher exists (K23). Held, unused, until
    // then.
    auto const vdso = ort::take_startup_handle(ours::HandleType::VmoVdso, 0);
    auto const debug = ort::take_startup_handle(ours::HandleType::SystemResource,
                                                u16(ours::RSRC_SYSTEM_DEBUG_BASE));
    if (vdso != ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: vdso vmo ok");
    } else {
        auto _ = ort::debug_line("init: started without the vdso vmo");
    }

    // The launcher: the BootFS mapped into init's own root region and made
    // executable with vmex. Without one there is no way to start anything:
    // the kernel loads no program once user space runs.
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);
    if (bootfs == ours::Uandle::Invalid || vmex == ours::Uandle::Invalid ||
        vdso == ours::Uandle::Invalid || vma == ours::Uandle::Invalid) {
        auto _ =
            ort::debug_line("init: cannot start programs (missing BootFS, vmex, vDSO or VmaRoot)");
        halt();
    }
    auto made = launch::make_bootfs_launcher(bootfs, vmex, vdso, vma,
                                             {.report = print_launch_error, .context = nullptr});
    if (made.is_err()) {
        report("init: cannot start programs, setting the launcher up = ", made.unwrap_err());
        halt();
    }
    auto const launcher = made.unwrap();

    ours::Uandle here = ours::Uandle::Invalid;
    ours::Uandle peer = ours::Uandle::Invalid;
    if (auto const status = ours::channel_create(ours::ChannelCreateOptions::None, &here, &peer);
        status != Status::Ok) {
        report("init: channel_create = ", status);
        halt();
    }
    auto shell = ours::Uandle::Invalid;
    if (auto const status = start_shell(launcher, job, bootfs, vmex, vdso, debug, peer, &shell);
        status != Status::Ok) {
        report("init: starting the shell = ", status);
        halt();
    }
    auto _ = ort::debug_line("init: launched shell");

    if (auto const status = ours::channel_write(here, ours::ChannelWriteOptions::None, PING.data(),
                                                static_cast<u32>(PING.size()), nullptr, 0);
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
    // The last one on the command line wins (ort::ProcessArgs::option()).
    if (auto const autorun = ort::process_args().option(init::AUTORUN_OPTION); !autorun.empty()) {
        auto _ = ort::debug_line({"init: autorun ", to_str(autorun)});
        if (auto const status = start_autorun(launcher, job, autorun); status != Status::Ok) {
            report("init: starting the autorun command = ", status);
        }
    }

    // Asleep until the shell writes back, or its end of the channel goes.
    auto observed = Signals::None;
    auto status = ours::object_wait_one(here, Signals::Readable | Signals::PeerClosed,
                                        ours::INSTANT_INFINITE, &observed);
    if (status != Status::Ok || !(observed & Signals::Readable)) {
        report("init: waiting for the shell = ", status);
        auto _ = ort::debug_line("init: the shell closed the channel without answering");
        halt();
    }

    char buffer[32] = {};
    u32 actual = 0;
    status = ours::channel_read(here, ours::ChannelReadOptions::None, buffer, nullptr,
                                sizeof(buffer), 0, &actual, nullptr);
    if (status != Status::Ok) {
        report("init: read = ", status);
        halt();
    }
    auto _ = ort::debug_line({"init: shell says \"", std::string_view(buffer, actual), "\""});

    // And then until the shell lets go of its end: it closes it as it checks
    // that closing a handle works, or its process dying closes it. Either way
    // this is the shell being done with init, not necessarily the shell being
    // gone -- that sign is TERMINATED on its process handle, below.
    status = ours::object_wait_one(here, Signals::PeerClosed, ours::INSTANT_INFINITE, &observed);
    if (status != Status::Ok) {
        report("init: waiting for the shell to finish = ", status);
        halt();
    }
    auto _ = ort::debug_line("init: shell closed its channel");

    // The shell is interactive: this sleeps until someone types `exit`.
    report_exit(shell);
    halt();
}
