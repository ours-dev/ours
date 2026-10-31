/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// init -- the first system service, and the root of the component tree.
///
/// userboot starts it on the runtime (//system/lib/ort), when the image
/// names it with `userboot.next=bin/init`, with the root job, the read-only
/// BootFS, VMEX, the vDSO's VMO, the Debug, Sampling and IOMMU resources, the roots
/// of the interrupt lines and the I/O ports, and its root region. It runs the
/// component framework (docs/design/261004/component-framework.md); since
/// step 15 of that plan it starts nothing but components, and it:
///
///   - ingests the BootFS into /boot (BootServer): a read-only slice per
///     file, made executable under bin/, and keeps VMEX for good, the only
///     process that holds it;
///   - resolves meta/root.cm, starts its eager children with the ELF runner
///     (Realm) -- routing_checks, then the shell -- each with a namespace of
///     /boot entries routed from its manifest and a /svc of the protocols it
///     uses, routed on every Open, and supervises them;
///   - serves its built-in protocols to the components routed them:
///     ours.process.Launcher (LauncherServer), through which the shell runs
///     every command, and the ours.resource protocols (ResourceServer), each
///     of which hands out a duplicate of a resource the root keeps --
///     ours.resource.Debug the Debug resource the shell reads the console
///     with, ours.resource.Sampling the Sampling resource a profiler creates
///     its session with, ours.resource.Iommu the IOMMU resource a bus driver
///     makes its BTIs from, ours.resource.Irq and ours.resource.Ioport the
///     roots of the interrupt lines and the I/O ports, for drivers;
///   - then waits for everything at once on one Loop: every /boot and /svc
///     connection, every Launcher and resource connection, and every
///     component's exit. It waits nowhere else (design I1).
///
/// After the eager children it also starts the program the image's
/// `ours.autorun.boot` option names, if there is one, so that an image can
/// run a command at boot without anyone typing it. The root resolves the
/// name in its own /boot and starts it with launch::start() by direct call
/// (design D25), as the ELF runner starts a component. The option reaches
/// init as an environment string: the kernel forwards it to userboot, and
/// userboot hands on what is not its own.
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
#include "launcher_server.hpp"
#include "pools.hpp"
#include "realm.hpp"
#include "resource_server.hpp"
#include <elfload/diagnostics.hpp>
#include <init/autorun.hpp>
#include <launch/bootfs.hpp>
#include <launch/launch.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/spin_hint.hpp>
#include <ort/startup.hpp>
#include <ours_resource/ours_resource.hpp>
#include <ustl/lazy_init.hpp>

namespace {
/// Prints "<what><status name>".
auto report(std::string_view const what, ours::Status const status) -> void {
    auto _ = ort::debug_line({what, std::string_view{ours::to_string(status)}});
}

[[noreturn]]
auto halt() -> void {
    // The spin hint is load-bearing, not decoration. C++'s forward-progress rule
    // makes a loop with no side effects undefined behaviour, and clang acts on
    // that licence: at -O2 it deletes `for (;;) {}` outright and lets control
    // fall off the end of the function. ort::spin_hint() is a side effect the
    // optimiser must keep.
    //
    // init halts rather than exiting: it is the process the system's other
    // programs are started from, and nothing yet would notice it gone.
    for (;;) {
        ort::spin_hint();
    }
}

auto to_str(std::string_view const text) -> std::string_view {
    return {text.data(), text.size()};
}

// What the root keeps, sized by its pools (pools.hpp): globals, because
// together they are far more than the stack, and constinit, because nothing
// runs a user program's dynamic initialisers. All but the Loop's slots are
// ustl::LazyInit, raw storage that main() builds the object in, for one or
// both of two reasons:
//   - The storage of /boot, the instance pool and the Launcher hold types
//     with destructors that are not trivial (ustl::Option among the first
//     two's members), which as plain globals would register those destructors
//     with __cxa_atexit, which the runtime does not have and the root never
//     needs, since it never exits. A connection that ends is reset, not
//     destroyed.
//   - The storage of the resource servers and of the Launcher holds
//     connections that are loop::Waiters, so each has a vptr (the Launcher's
//     two), which is not zero. A constinit object with any non-zero byte lies
//     in .data, which init's file stores whole, where an all-zero one lies in
//     .bss, which it does not: the five resource storages would put their
//     4 KiB message buffers, all zeros, into the file, 22 KiB of it, and the
//     Launcher's pool 28 KiB. As LazyInit they are zeros in .bss, and main()
//     writes the vptrs when it builds them. The state of each Launcher
//     connection holds hooks that point back at its own entry, which its
//     constructor wires wherever it runs: the entry is built in place and
//     never moves. Nothing about a server needs its storage to exist at
//     compile time.
constinit ustl::LazyInit<init::BootServer::Storage> BOOT_STORAGE;
constinit ustl::LazyInit<std::array<init::Instance, init::MAX_INSTANCES>> INSTANCES;
constinit std::array<loop::Slot, init::LOOP_SLOTS> SLOTS{};
constinit ustl::LazyInit<init::LauncherServer::Storage> LAUNCHER_STORAGE;
constinit ustl::LazyInit<init::ResourceServer::Storage> DEBUG_RESOURCE_STORAGE;
constinit ustl::LazyInit<init::ResourceServer::Storage> IRQ_RESOURCE_STORAGE;
constinit ustl::LazyInit<init::ResourceServer::Storage> IOPORT_RESOURCE_STORAGE;
constinit ustl::LazyInit<init::ResourceServer::Storage> MMIO_RESOURCE_STORAGE;
constinit ustl::LazyInit<init::ResourceServer::Storage> IOMMU_RESOURCE_STORAGE;
constinit ustl::LazyInit<init::ResourceServer::Storage> SAMPLING_RESOURCE_STORAGE;
constinit ustl::LazyInit<init::ResourceServer::Storage> ACPI_STORAGE;
constinit ustl::LazyInit<init::ResourceServer::Storage> ROOT_JOB_STORAGE;

/// launch::start()'s failures, for the Launcher and autorun, on the debug
/// console. Only the errors: the shell's printer also shows every import
/// bound, which init's log does not need.
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

/// Starts the autorun |command| under the root job by direct call (design
/// D25), split by init::AutorunCommand::parse(): its first `+`-separated
/// word is the program's BootFS name, which the root looks up in its own
/// /boot, and all of them, the name included, are its argv.
///
/// The program gets argv, its root region and its handle to itself, and
/// nothing else: no job, since nothing it could be asked to do yet starts
/// processes, and no namespace, since it is not a component. That is what
/// the standalone launcher gave it before step 15, and ProcSelf, which every
/// starter adds. What it prints goes to the debug log like everyone else's.
/// The root does not wait for it: it keeps no handle to the process.
///
/// Like the ELF runner's, the program must be one the root made executable
/// at ingest: launch::start() maps it with Execute, which a slice outside
/// bin/ lacks (AccessDenied).
auto start_autorun(init::BootServer const &boot, init::Environment const &environment,
                   std::string_view const command) -> ours::Status {
    using ours::Status;

    init::AutorunCommand parsed;
    if (auto const status = init::AutorunCommand::parse(command, &parsed); status != Status::Ok) {
        return status;
    }
    auto const file = boot.file(parsed.name());
    if (file.is_err()) {
        return file.unwrap_err();
    }

    // The processargs first, so that one that cannot be encoded starts
    // nothing. Its two handles, the root region and the process, come from
    // start().
    u32 const infos[] = {ours::handle_info(ours::HandleType::VmaRoot, 0),
                         ours::handle_info(ours::HandleType::ProcSelf, 0)};
    std::array<u8, ours::CHANNEL_MAX_MSG_BYTES> message{};
    u32 length = 0;
    if (auto const status =
            ours::encode_process_args(message, infos, parsed.words(), {}, {}, &length);
        status != Status::Ok) {
        return status;
    }
    ours::Uandle here = ours::Uandle::Invalid;
    ours::Uandle there = ours::Uandle::Invalid;
    if (auto const status = ours::channel_create(ours::ChannelCreateOptions::None, &here, &there);
        status != Status::Ok) {
        return status;
    }
    auto const started = launch::start({
        .job = environment.job,
        .name = parsed.name(),
        .file = file->vmo,
        .vma = environment.vma,
        .vdso = environment.vdso,
        .vdso_file = environment.vdso_file,
        .bootstrap = there,
        .diagnostics = {.report = print_launch_error, .context = nullptr},
    });
    if (started.is_err()) {
        auto _ = ours::handle_close(here);
        return started.unwrap_err();
    }
    // The process handle goes to the program itself, as its ProcSelf with
    // every right: the root does not wait for it and has no use for one.
    auto handles = std::array{started->root_vma, started->process};
    auto const written =
        ours::channel_write(here, ours::ChannelWriteOptions::None, message.data(), length,
                            handles.data(), static_cast<u32>(handles.size()));
    // The write took both, whatever it answered; a program whose processargs
    // never came finds its bootstrap channel closed and exits.
    auto _ = ours::handle_close(here);
    return written;
}
} // namespace

auto main(int, char **) -> int {
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
    // The Sampling resource, kept like Debug: a component gets a duplicate
    // only through ours.resource.Sampling, when the manifests route it one.
    auto const sampling = ort::take_startup_handle(ours::HandleType::SystemResource,
                                                   u16(ours::RSRC_SYSTEM_SAMPLING_BASE));
    // The IOMMU resource, kept like Sampling: a component gets a duplicate
    // only through ours.resource.Iommu, when the manifests route it one.
    auto const iommu = ort::take_startup_handle(ours::HandleType::SystemResource,
                                                u16(ours::RSRC_SYSTEM_IOMMU_BASE));
    if (iommu == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: started without the IOMMU resource");
    }
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);
    if (bootfs == ours::Uandle::Invalid || vmex == ours::Uandle::Invalid ||
        vdso == ours::Uandle::Invalid || vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: cannot start (missing BootFS, vmex, vDSO or VmaRoot)");
        halt();
    }
    // The roots of the interrupt lines, the I/O ports and the physical address
    // space, which the root keeps, as it keeps VMEX: a component gets a
    // duplicate only through ours.resource.Irq, ours.resource.Ioport or
    // ours.resource.Mmio, when the manifests route it one, as a driver's do. A root without them still starts the rest, as
    // one without the Debug resource does: it says what it has, and a Get it
    // cannot answer says why in its reply.
    auto const irq = ort::take_startup_handle(ours::HandleType::IrqResource, 0);
    auto const ioport = ort::take_startup_handle(ours::HandleType::IoportResource, 0);
    if (irq != ours::Uandle::Invalid && ioport != ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: holding the IRQ and I/O port resources");
    } else {
        auto _ = ort::debug_line("init: started without the IRQ or I/O port resource");
    }
    auto const mmio = ort::take_startup_handle(ours::HandleType::MmioResource, 0);
    if (mmio != ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: holding the MMIO resource");
    } else {
        auto _ = ort::debug_line("init: started without the MMIO resource");
    }
    // The ACPI tables, kept like the roots: a component gets a duplicate only
    // through ours.boot.Acpi, when the manifests route it one. A platform that
    // read no tables hands none, which is not a fault: a Get then answers what
    // every ResourceServer answers for a handle it does not hold, BadHandle, and
    // the line below says why.
    auto const acpi = ort::take_startup_handle(ours::HandleType::VmoAcpi, 0);
    if (acpi != ours::Uandle::Invalid) {
        auto _ = ort::debug_line("init: holding the ACPI tables");
    } else {
        auto _ = ort::debug_line("init: started without ACPI tables");
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
    loop::Loop loop(port, SLOTS);
    init::BootServer boot(*BOOT_STORAGE.init(), loop);
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
    init::LauncherServer launcher_server(*LAUNCHER_STORAGE.init(), loop, environment,
                                         {.report = print_launch_error, .context = nullptr});
    init::ResourceServer debug_resource_server(*DEBUG_RESOURCE_STORAGE.init(), loop,
                                               ours_resource::DEBUG_PROTOCOL, debug);
    init::ResourceServer irq_resource_server(*IRQ_RESOURCE_STORAGE.init(), loop,
                                             ours_resource::IRQ_PROTOCOL, irq);
    init::ResourceServer ioport_resource_server(*IOPORT_RESOURCE_STORAGE.init(), loop,
                                                ours_resource::IOPORT_PROTOCOL, ioport);
    init::ResourceServer mmio_resource_server(*MMIO_RESOURCE_STORAGE.init(), loop,
                                              ours_resource::MMIO_PROTOCOL, mmio);
    init::ResourceServer iommu_resource_server(*IOMMU_RESOURCE_STORAGE.init(), loop,
                                               ours_resource::IOMMU_PROTOCOL, iommu);
    init::ResourceServer sampling_resource_server(*SAMPLING_RESOURCE_STORAGE.init(), loop,
                                                  ours_resource::SAMPLING_PROTOCOL, sampling);
    // The ACPI tables' VMO, read only: each Get answers a duplicate of it.
    init::ResourceServer acpi_server(*ACPI_STORAGE.init(), loop, ours_resource::ACPI_PROTOCOL,
                                     acpi);
    // The root job the root was started with, which stays the root's: each
    // Get answers a duplicate with every right of it.
    init::ResourceServer root_job_server(*ROOT_JOB_STORAGE.init(), loop,
                                         ours_resource::ROOT_JOB_PROTOCOL, job);
    init::Realm realm(*INSTANCES.init(), boot, loop, environment,
                      {
                          .launcher = &launcher_server,
                          .debug_resource = &debug_resource_server,
                          .irq_resource = &irq_resource_server,
                          .ioport_resource = &ioport_resource_server,
                          .mmio_resource = &mmio_resource_server,
                          .iommu_resource = &iommu_resource_server,
                          .sampling_resource = &sampling_resource_server,
                          .acpi = &acpi_server,
                          .root_job = &root_job_server,
                      });
    if (auto const status = realm.resolve_root(); status != Status::Ok) {
        report("init: cannot resolve meta/root.cm = ", status);
        halt();
    }
    realm.start_root();

    // The autorun program, after the eager children, so that it runs beside
    // the shell rather than before it. A failure is reported and nothing
    // more; it is the command that failed, not the system, and the shell is
    // already running. The last one on the command line wins
    // (ort::ProcessArgs::option()).
    if (auto const autorun = ort::process_args().option(init::AUTORUN_OPTION); !autorun.empty()) {
        auto _ = ort::debug_line({"init: autorun ", to_str(autorun)});
        if (auto const status = start_autorun(boot, environment, autorun); status != Status::Ok) {
            report("init: starting the autorun command = ", status);
        }
    }

    // Everything from here is a callback: /boot's and /svc's connections,
    // the built-in protocols' and the components' exits. run() never returns
    // while the port works.
    report("init: the loop ended = ", loop.run(ours::INSTANT_INFINITE));
    halt();
}
