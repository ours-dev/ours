/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// init -- the first system service, and the root of the component tree.
///
/// userboot starts it on the runtime (//system/lib/ort), when the image
/// names it with `userboot.next=bin/init`, with the root job, the read-only
/// BootFS, VMEX, the vDSO's VMO, the Debug resource and its root region. It
/// is growing into ours's counterpart of Fuchsia's component_manager
/// (docs/design/261004/component-framework.md); this is step 14 of that
/// plan, where it:
///
///   - ingests the BootFS into /boot (BootServer): a read-only slice per
///     file, made executable under bin/, and keeps VMEX for good;
///   - resolves meta/root.cm, starts its eager children with the ELF runner
///     (Realm), each with a namespace of /boot entries routed from its
///     manifest and a /svc of the protocols it uses, routed on every Open,
///     and supervises them;
///   - serves its two built-in protocols, fuchsia.process.Launcher
///     (LauncherServer) and fuchsia.kernel.DebugResource
///     (DebugResourceServer), to the components routed them;
///   - still starts the standalone launcher, with launch::start() on the
///     launcher's file from its own /boot, and the shell through it, as
///     before, until steps 15 and 16 move the shell into the tree and
///     remove the standalone launcher;
///   - then waits for everything at once on one Loop: every /boot and /svc
///     connection, every Launcher and DebugResource connection, every
///     component's exit, the shell's answer to its ping and the shell's
///     exit.
///
/// Between the shell and the Loop it also starts the program the image's
/// `ours.autorun.boot` option names, if there is one -- Zircon's
/// `zircon.autorun.boot`, which console-launcher, the component that starts
/// Zircon's shell, runs in the same place. The option reaches init as an
/// environment string: the kernel forwards it to userboot, and userboot
/// hands on what is not its own.
///
/// A root that cannot start -- no BootFS, VMEX, vDSO or root region, a BootFS
/// it cannot serve, or no meta/root.cm that decodes -- says so in one line and
/// halts (D28). It never returns from main: nothing yet would notice it gone.

#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>
#include <string_view>

#include "boot_server.hpp"
#include "debug_resource_server.hpp"
#include "launcher_server.hpp"
#include "pools.hpp"
#include "programs.hpp"
#include "realm.hpp"
#include <elfload/diagnostics.hpp>
#include <init/autorun.hpp>
#include <launch/bootfs.hpp>
#include <launch/client.hpp>
#include <launch/launch.hpp>
#include <launch/protocol.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ustl/lazy_init.hpp>

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

// What the root keeps, sized by its pools (pools.hpp): globals, because
// together they are far more than the stack, and constinit, because nothing
// runs a user program's dynamic initialisers. The storage of /boot and the
// instance pool hold types with destructors (ustl::Option among their
// members), which as plain globals would register those destructors with
// __cxa_atexit, which the runtime does not have and the root never needs,
// since it never exits; ustl::LazyInit is raw storage that main() builds
// them in instead.
constinit ustl::LazyInit<init::BootServer::Storage> boot_storage;
constinit ustl::LazyInit<std::array<init::Instance, init::MAX_INSTANCES>> instances;
constinit std::array<loop::Slot, init::LOOP_SLOTS> slots{};
constinit init::DebugResourceServer::Storage debug_resource_storage{};

// The Launcher's connections are built at compile time instead: each one's
// state holds hooks that point back at its own entry, which constinit can
// write into the image, and nothing need be built in main(). Their state's
// destructor is not trivial, and [[clang::no_destroy]] keeps the compiler
// from registering it with __cxa_atexit: the root never exits, and a
// connection that ends is reset, not destroyed.
[[clang::no_destroy]]
constinit init::LauncherServer::Storage launcher_storage{};

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

/// Starts the launcher (bin/launcher) under |job| with launch::start(), on
/// |file|, the launcher's executable VMO from init's own /boot: the program
/// init starts itself besides its components, until the Launcher moves into
/// the root (step 14) and the standalone one goes (step 16). |vma| is init's
/// root region, where start() reads the program, and |vdso_file| the vDSO's
/// bytes, which it binds the program against.
///
/// It gets |connection| (User0), the far end of init's own connection to it,
/// and what starting programs takes: a duplicate of the BootFS, of VMEX and
/// of the vDSO's VMO, and its root region. |connection| leaves init whatever
/// the answer. The processargs is written after the launcher exists, since
/// its root region goes in it -- the order the library keeps.
auto start_launcher(ours::Uandle const file, ours::Uandle const vma,
                    std::span<u8 const> const vdso_file, ours::Uandle const job,
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
    auto const started = launch::start({
        .job = job,
        .name = user_programs::LAUNCHER,
        .file = file,
        .vma = vma,
        .vdso = vdso,
        .vdso_file = vdso_file,
        .bootstrap = there,
        .diagnostics = {.report = print_launch_error, .context = nullptr},
    });
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
/// User0 startup handle, duplicates of the job, the BootFS (for `ls`) and
/// the vDSO's VMO (for its vDSO-mapping check), the System Debug resource
/// for COM1, and a connection of its own to the launcher
/// (launch::protocol::CONNECTION), through which it starts its commands. No
/// VMEX: the launcher is the only process that holds it.
///
/// The liveness channel is separate from the bootstrap channel, which the
/// runtime closes as soon as it has read processargs. The shell answers init's
/// ping on the liveness channel, then keeps running its command loop.
///
/// |peer|, |debug| and the duplicates leave init with the request, whatever
/// the answer; init keeps its originals. The shell's process handle comes
/// back in |process|.
auto start_shell(launch::Client const &launcher, ours::Uandle const job, ours::Uandle const bootfs,
                 ours::Uandle const vdso, ours::Uandle debug, ours::Uandle peer,
                 ours::Uandle *const process) -> ours::Status {
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
    auto const connection = launcher.connect();
    if (connection.is_err()) {
        close_all({handed, count});
        auto _ = ours::handle_close(debug);
        return connection.unwrap_err();
    }
    handed[count] = connection.unwrap();
    infos[count++] = ours::handle_info(launch::protocol::CONNECTION, 0);
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

/// The channel the shell answers init's ping on, as its Loop slot sees it.
/// One slot serves both of init's old waits on it: the answer, then the
/// shell letting go of its end.
struct ShellChannel {
    ours::Uandle channel = ours::Uandle::Invalid;
    bool answered = false;
};

/// The shell's channel slot: its answer to the ping, printed, then, once the
/// shell lets go of its end, that. Letting go is not the shell being gone --
/// it closes its end as it checks that closing a handle works -- which is
/// what the process slot below says.
auto on_shell_channel(void *const context, ours::Signals const observed) -> loop::Action {
    auto &shell = *static_cast<ShellChannel *>(context);
    if (!(observed & ours::Signals::Readable)) {
        if (shell.answered) {
            auto _ = ort::debug_line("init: shell closed its channel");
        } else {
            auto _ = ort::debug_line("init: the shell closed the channel without answering");
        }
        return loop::Action::Close;
    }
    char buffer[32] = {};
    u32 actual = 0;
    if (auto const status =
            ours::channel_read(shell.channel, ours::ChannelReadOptions::None, buffer, nullptr,
                               sizeof(buffer), 0, &actual, nullptr);
        status != ours::Status::Ok) {
        report("init: read = ", status);
        return loop::Action::Close;
    }
    auto _ = ort::debug_line({"init: shell says \"", std::string_view(buffer, actual), "\""});
    shell.answered = true;
    return loop::Action::Rearm;
}

/// The shell's process slot: the code it ended with, said on the console --
/// TERMINATED first, since until then a kill may still change it, then
/// object_get_info. The shell is interactive, so this fires when someone
/// types `exit`.
auto on_shell_exit(void *const context, ours::Signals) -> loop::Action {
    auto const process = *static_cast<ours::Uandle const *>(context);
    ours::InfoProcess info{};
    if (auto const status = ours::object_get_info(process, ours::InfoTopic::Process, &info,
                                                  sizeof(info), nullptr, nullptr);
        status != ours::Status::Ok) {
        report("init: object_get_info(the shell) = ", status);
        return loop::Action::Close;
    }
    char code[21];
    auto _ =
        ort::debug_line({"init: shell exited with ", ort::signed_decimal(info.return_code, code)});
    return loop::Action::Close;
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
    // VMEX, which the root keeps for life (design "Events: boot", step 2):
    // only it can make a VMO executable, and it makes the files under bin/
    // executable at ingest.
    auto const vmex = ort::take_startup_handle(ours::HandleType::SystemResource,
                                               u16(ours::RSRC_SYSTEM_VMEX_BASE));
    auto const vdso = ort::take_startup_handle(ours::HandleType::VmoVdso, 0);
    auto const debug = ort::take_startup_handle(ours::HandleType::SystemResource,
                                                u16(ours::RSRC_SYSTEM_DEBUG_BASE));
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);
    if (bootfs == ours::Uandle::Invalid || vmex == ours::Uandle::Invalid ||
        vdso == ours::Uandle::Invalid || vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: cannot start (missing BootFS, vmex, vDSO or VmaRoot)");
        halt();
    }

    // The BootFS, mapped read-only for good: /boot's Read copies from it,
    // and its names and manifests are read in place.
    auto const mapped =
        launch::map_bootfs(bootfs, vma, {.report = print_launch_error, .context = nullptr});
    if (mapped.is_err()) {
        report("init: cannot map the BootFS = ", mapped.unwrap_err());
        halt();
    }
    auto port = ours::Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        report("init: port_create = ", status);
        halt();
    }
    loop::Loop loop(port, slots);
    init::BootServer boot(*boot_storage.init(), loop);
    auto const ingested = boot.ingest(mapped.unwrap(), bootfs, vmex);
    if (ingested.is_err()) {
        report("init: cannot serve the BootFS = ", ingested.unwrap_err());
        halt();
    }
    char files[10];
    char executable[10];
    auto _ = ort::debug_line({"init: /boot has ", ort::decimal(ingested->files, files), " files, ",
                              ort::decimal(ingested->executable, executable), " executable"});

    // What every start needs besides the program: the vDSO's bytes, which
    // programs are bound against, read from /boot like any file.
    auto const vdso_file = boot.file("vdso");
    if (vdso_file.is_err()) {
        report("init: cannot start programs, finding the vDSO = ", vdso_file.unwrap_err());
        halt();
    }

    // The built-in protocols, served once routes lead to them, and the
    // component tree: meta/root.cm, then its eager children.
    auto const environment = init::Environment{
        .job = job,
        .vdso = vdso,
        .vdso_file = vdso_file->bytes,
        .vma = vma,
    };
    init::LauncherServer launcher_server(launcher_storage, loop, environment,
                                         {.report = print_launch_error, .context = nullptr});
    init::DebugResourceServer debug_resource_server(debug_resource_storage, loop, debug);
    init::Realm realm(*instances.init(), boot, loop, environment,
                      {.launcher = &launcher_server, .debug_resource = &debug_resource_server});
    if (auto const status = realm.resolve_root(); status != Status::Ok) {
        report("init: cannot resolve meta/root.cm = ", status);
        halt();
    }
    realm.start_root();

    // The standalone launcher and the shell, as before the root existed,
    // until the shell becomes a component (step 15).
    auto const launcher_file = boot.file(user_programs::LAUNCHER);
    if (launcher_file.is_err()) {
        report("init: cannot find the launcher = ", launcher_file.unwrap_err());
        halt();
    }
    ours::Uandle connection = ours::Uandle::Invalid;
    ours::Uandle connection_there = ours::Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &connection, &connection_there);
        status != Status::Ok) {
        report("init: channel_create = ", status);
        halt();
    }
    if (auto const status = start_launcher(launcher_file->vmo, vma, vdso_file->bytes, job, bootfs,
                                           vmex, vdso, connection_there);
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
    // The shell gets a duplicate of the Debug resource: the root keeps its
    // own for DebugResource.Get.
    auto shell_debug = ours::Uandle::Invalid;
    if (debug != ours::Uandle::Invalid) {
        if (auto const status =
                ours::handle_duplicate(debug, ours::Rights::SameRights, &shell_debug);
            status != Status::Ok) {
            report("init: duplicating the Debug resource = ", status);
        }
    }
    auto shell = ours::Uandle::Invalid;
    if (auto const status = start_shell(launcher, job, bootfs, vdso, shell_debug, peer, &shell);
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
    // (LaunchAutorun() in its main.cc, just before RunSerialConsole()).
    // A failure is reported and nothing more; it is the command that failed,
    // not the system, and the shell is already running.
    // The last one on the command line wins (ort::ProcessArgs::option()).
    if (auto const autorun = ort::process_args().option(init::AUTORUN_OPTION); !autorun.empty()) {
        auto _ = ort::debug_line({"init: autorun ", to_str(autorun)});
        if (auto const status = start_autorun(launcher, job, autorun); status != Status::Ok) {
            report("init: starting the autorun command = ", status);
        }
    }

    // init's two old blocking waits on the shell become slots: the answer to
    // the ping and the shell letting go of the channel on one, TERMINATED on
    // its process on the other. The Loop owns both handles from here.
    auto shell_channel = ShellChannel{.channel = here, .answered = false};
    if (auto const added = loop.add(here, Signals::Readable | Signals::PeerClosed,
                                    &on_shell_channel, &shell_channel);
        added.is_err()) {
        report("init: waiting for the shell = ", added.unwrap_err());
    }
    if (auto const added = loop.add(shell, Signals::Terminated, &on_shell_exit, &shell);
        added.is_err()) {
        report("init: watching the shell = ", added.unwrap_err());
    }

    // Everything from here is a callback: /boot's connections, the
    // components' exits and the shell's. run() never returns while the port
    // works.
    report("init: the loop ended = ", loop.run(ours::INSTANT_INFINITE));
    halt();
}
