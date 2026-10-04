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
/// init starts one program itself, with the launcher library
/// (launch::launch_from_bootfs()): bin/launcher, the service that starts
/// every other one. It hands the launcher the BootFS, vmex and the vDSO's
/// VMO, and keeps a connection to it; the shell and the autorun command are
/// then Launch requests on that connection (launch::Client), and once user
/// space is running the kernel loads no program at all. The launcher hands
/// back the process handle: once the shell has let go of init's channel,
/// init waits for it to terminate and says what it ended with
/// (object_get_info). Without what starting the launcher needs -- the
/// BootFS, vmex, the vDSO's VMO and init's own root region -- init starts
/// nothing and halts.
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
#include <launch/client.hpp>
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

auto close_all(std::span<ours::Uandle const> const handles) -> void {
    for (auto const handle: handles) {
        auto _ = ours::handle_close(handle);
    }
}

auto to_str(std::string_view const text) -> std::string_view {
    return {text.data(), text.size()};
}

/// Reads the BootFS header through vmo_read and says how many files it
/// holds -- the first time a program looks at the BootFS the kernel built
/// for it (design K12), and the first vmo_read of a real VMO.
///
/// init keeps the handle; the launcher and the shell each get a duplicate.
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

/// Starts the launcher (bin/launcher) under |job| with the library: the one
/// program init starts itself, since every later start is a request to it.
///
/// It gets |connection| (User0), the far end of init's own connection to it,
/// and what starting programs takes: a duplicate of the BootFS, of VMEX and
/// of the vDSO's VMO, and its root region. |connection| leaves init whatever
/// the answer. The processargs is written after the launcher exists, since
/// its root region goes in it -- the order the library keeps.
auto start_launcher(launch::BootfsLauncher const &library, ours::Uandle const job,
                    ours::Uandle const bootfs, ours::Uandle const vmex, ours::Uandle const vdso,
                    ours::Uandle const connection) -> ours::Status {
    using ours::Status;

    ours::Uandle handed[5] = {connection};
    u32 count = 1;
    for (auto const original: {bootfs, vmex, vdso}) {
        if (auto const status =
                ours::handle_duplicate(original, ours::Rights::SameRights, &handed[count]);
            status != Status::Ok) {
            close_all({handed, count});
            return status;
        }
        count += 1;
    }
    u32 const infos[] = {
        ours::handle_info(ours::HandleType::User0, 0),
        ours::handle_info(ours::HandleType::VmoBootfs, 0),
        ours::handle_info(ours::HandleType::SystemResource, u16(ours::RSRC_SYSTEM_VMEX_BASE)),
        ours::handle_info(ours::HandleType::VmoVdso, 0),
        ours::handle_info(ours::HandleType::VmaRoot, 0),
    };
    std::string_view const args[] = {user_programs::LAUNCHER};
    u8 message[256];
    u32 length = 0;
    if (auto const status = ours::encode_process_args(message, infos, args, {}, {}, &length);
        status != Status::Ok) {
        close_all({handed, count});
        return status;
    }

    ours::Uandle here = ours::Uandle::Invalid;
    ours::Uandle there = ours::Uandle::Invalid;
    if (auto const status = ours::channel_create(ours::ChannelCreateOptions::None, &here, &there);
        status != Status::Ok) {
        close_all({handed, count});
        return status;
    }
    auto const started =
        launch::launch_from_bootfs(library, job, user_programs::LAUNCHER, there,
                                   {.report = print_launch_error, .context = nullptr});
    if (started.is_err()) {
        close_all({handed, count});
        auto _ = ours::handle_close(here);
        return started.unwrap_err();
    }
    handed[count++] = started.unwrap().root_vma;
    auto const written =
        ours::channel_write(here, ours::ChannelWriteOptions::None, message, length, handed, count);
    auto _ = ours::handle_close(here);
    // init does not wait for the launcher; the process lives on without this
    // handle.
    auto _ = ours::handle_close(started.unwrap().process);
    return written;
}

/// Starts the autorun |command| under |job| through the launcher, split by
/// init::AutorunCommand::parse(): its first `+`-separated word is the BootFS
/// name, and all of them, the name included, are its argv.
///
/// The program gets argv and nothing else: no job, since nothing it could be
/// asked to do yet starts processes, and no BootFS, which init keeps. What it
/// prints goes to the debug log like everyone else's. init does not wait for
/// it, so the process handle the launcher hands back is closed at once:
/// closing it does not stop a process that has started.
auto start_autorun(launch::Client const &launcher, ours::Uandle const job,
                   std::string_view const command) -> ours::Status {
    using ours::Status;

    init::AutorunCommand parsed;
    if (auto const status = init::AutorunCommand::parse(command, &parsed); status != Status::Ok) {
        return status;
    }
    auto const started = launcher.launch(job, to_str(parsed.name()), parsed.words(), {}, {}, {});
    if (started.is_err()) {
        return started.unwrap_err();
    }
    auto _ = ours::handle_close(started.unwrap());
    return Status::Ok;
}

/// Starts the shell under |job| through the launcher, with |peer| as its
/// User0 startup handle, duplicates of the job, the BootFS, VMEX and the
/// vDSO's VMO, and the System Debug resource for COM1.
///
/// The liveness channel is separate from the bootstrap channel, which the
/// runtime closes as soon as it has read processargs. The shell answers init's
/// ping on the liveness channel, then keeps running its command loop.
///
/// |peer|, |debug| and the duplicates leave init with the request, whatever
/// the answer; init keeps its originals. The shell's process handle comes
/// back in |process|.
auto start_shell(launch::Client const &launcher, ours::Uandle const job, ours::Uandle const bootfs,
                 ours::Uandle const vmex, ours::Uandle const vdso, ours::Uandle debug,
                 ours::Uandle peer, ours::Uandle *const process) -> ours::Status {
    using ours::Status;

    if (debug == ours::Uandle::Invalid) {
        auto _ = ours::handle_close(peer);
        return Status::BadHandle;
    }

    ours::Uandle handed[6] = {peer};
    u32 infos[6] = {ours::handle_info(ours::HandleType::User0, 0)};
    u32 count = 1;
    struct Duplicated {
        ours::Uandle original;
        u32 info;
    };
    Duplicated const duplicated[] = {
        {job, ours::handle_info(ours::HandleType::JobDefault, 0)},
        {bootfs, ours::handle_info(ours::HandleType::VmoBootfs, 0)},
        {vmex,
         ours::handle_info(ours::HandleType::SystemResource, u16(ours::RSRC_SYSTEM_VMEX_BASE))},
        {vdso, ours::handle_info(ours::HandleType::VmoVdso, 0)},
    };
    for (auto const &[original, info]: duplicated) {
        if (auto const status =
                ours::handle_duplicate(original, ours::Rights::SameRights, &handed[count]);
            status != Status::Ok) {
            close_all({handed, count});
            auto _ = ours::handle_close(debug);
            return status;
        }
        infos[count++] = info;
    }
    handed[count] = debug;
    infos[count++] =
        ours::handle_info(ours::HandleType::SystemResource, u16(ours::RSRC_SYSTEM_DEBUG_BASE));

    std::string_view const args[] = {user_programs::SHELL};
    auto const started =
        launcher.launch(job, user_programs::SHELL, args, {}, {handed, count}, {infos, count});
    if (started.is_err()) {
        return started.unwrap_err();
    }
    *process = started.unwrap();
    return Status::Ok;
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
    // (design D3). init uses it to start the launcher and hands the launcher
    // a duplicate; the shell still gets one too, until it launches through
    // the launcher.
    auto const vmex = ort::take_startup_handle(ours::HandleType::SystemResource,
                                               u16(ours::RSRC_SYSTEM_VMEX_BASE));
    if (vmex != ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: vmex resource ok");
    } else {
        auto _ = ort::debug_line("init: started without vmex");
    }

    // The vDSO's VMO, also handed on by userboot: mapped into the launcher by
    // the library, and duplicated to the launcher and the shell.
    auto const vdso = ort::take_startup_handle(ours::HandleType::VmoVdso, 0);
    auto const debug = ort::take_startup_handle(ours::HandleType::SystemResource,
                                                u16(ours::RSRC_SYSTEM_DEBUG_BASE));
    if (vdso != ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: vdso vmo ok");
    } else {
        auto _ = ort::debug_line("init: started without the vdso vmo");
    }

    // The library: the BootFS mapped into init's own root region and made
    // executable with vmex, for the one program init starts itself. Without
    // it there is no way to start anything: the kernel loads no program once
    // user space runs.
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
    auto const library = made.unwrap();

    // The launcher first, with the library; everything after it through it.
    ours::Uandle connection = ours::Uandle::Invalid;
    ours::Uandle connection_there = ours::Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &connection, &connection_there);
        status != Status::Ok) {
        report("init: channel_create = ", status);
        halt();
    }
    if (auto const status = start_launcher(library, job, bootfs, vmex, vdso, connection_there);
        status != Status::Ok) {
        report("init: starting the launcher = ", status);
        halt();
    }
    auto _ = ort::debug_line("init: launched the launcher");
    auto const launcher = launch::Client(connection);

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
