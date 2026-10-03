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
/// BootFS, and -- as environment strings -- the image's user-space options
/// (collect_user_options() in //kernel/lib/userabi/src/userboot.cpp), which
/// is how this program sees a command line it has no other way to reach.
/// The `userboot.*` ones are its own; the rest (`ours.autorun.boot`, which
/// init reads) it hands on to the next program as that program's
/// environment. Zircon's userboot reads its options out of the ZBI's
/// CMDLINE item instead (get_options() in main.rs) and hands the whole ZBI
/// on; the kernel forwarding tokens spares both programs a boot-container
/// parser.
///
/// It starts the next program itself, with the user-space launcher
/// (launch::launch_from_bootfs(), U8): the BootFS mapped into its own address
/// space and made executable with vmex. The kernel loads this program and
/// no other; Zircon's userboot likewise loads its next program from the
/// BootFS itself (launch_program() in main.rs). The mapping and the
/// executable copy go with this
/// process when it exits.

#include <string_view>

#include <elfload/diagnostics.hpp>
#include <launch/bootfs.hpp>
#include <launch/launch.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

namespace {
/// The option naming the program to start next. Only the `key=value` form
/// means anything, since a bare `userboot.next` names no program;
/// ort::ProcessArgs::option() reads both as empty. Zircon's key of the
/// same name (parse_option() in main.rs).
constexpr std::string_view NEXT_OPTION{"userboot.next"};

/// The family of options that are this program's own and stop here; every
/// other environment string is the next program's and is handed on.
constexpr std::string_view USERBOOT_OPTION_PREFIX{"userboot."};

/// The environment strings the next program gets: this program's own less
/// its `userboot.*` options.
///
/// Views into this process's processargs, which the runtime keeps for the
/// life of the process; held only until start_next() has copied them into
/// the next program's message.
///
/// Room for every option the kernel may relay (ours::MAX_USER_OPTIONS), so a
/// subset of them always fits.
struct HandedOn {
    std::string_view entries[ours::MAX_USER_OPTIONS];
    u32 count{0};
};

/// Prints "<what><status name>".
auto report(std::string_view const what, ours::Status const status) -> void {
    auto _ = ort::debug_line({what, std::string_view{ours::to_string(status)}});
}

auto to_str(std::string_view const text) -> std::string_view {
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
    // Only the proof-of-life image comes here; every path that was asked to
    // start something ends in process_exit instead (see main). With nothing
    // to hand on, staying keeps the one thing left to observe -- the timer
    // interrupts the kernel goes on taking from user space -- as evidence
    // that ring 3 resumed after its last system call.
    for (;;) {
        __asm__ volatile("pause");
    }
}

/// The environment strings that are not this program's own options.
///
/// Not a filter the kernel could apply for it: the kernel sends one message,
/// to this program, and has no say in what the next one gets. Zircon's
/// userboot passes on everything instead -- the whole ZBI -- but a program
/// there asks component_manager for the options it wants; here the
/// environment *is* the options, so what `userboot.next` was is none of
/// init's business. One that does not fit -- only possible if something
/// other than the kernel wrote this program's message -- is dropped and
/// named, as the kernel does with a token it has no room for.
auto collect_handed_on() -> HandedOn {
    HandedOn handed_on;
    for (auto const entry: ort::process_args().environ()) {
        if (entry.starts_with(USERBOOT_OPTION_PREFIX)) {
            continue;
        }
        if (handed_on.count == ours::MAX_USER_OPTIONS) {
            auto _ =
                ort::debug_line({"userboot: dropping ", to_str(entry), ": no room to hand it on"});
            continue;
        }
        handed_on.entries[handed_on.count++] = entry;
    }
    return handed_on;
}

/// The launcher's failures, on the debug console: only the errors, each
/// naming the step.
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
        {"userboot: launch failed: ", {report.what.data(), report.what.size()}, separator, name});
}

/// Starts |name| from the BootFS with |launcher| under |job|, handing it
/// |job|, |bootfs|, |vmex|, |vdso| and |debug| as its startup handles --
/// exactly what the kernel handed this program, so the next program starts
/// as it would have had the kernel started it directly. argv is {|name|};
/// the environment is |environ|, the options that were not this program's.
///
/// The process is started before its processargs are written, the reverse
/// of the kernel's order (make_bootstrap()): |job| has to go into the
/// message, and channel_write takes every handle it sends out of this
/// process, so written first it would leave nothing to launch under. The
/// other way round costs nothing -- the runtime waits for READABLE before
/// it reads -- and saves a handle_duplicate that would only leave userboot
/// holding a second root job handle it has no use for.
///
/// On success the handles have left this process. On failure to launch they
/// are still here, and the caller decides what to do with them; a failure
/// after the launch loses them with the message, and the new process,
/// finding PEER_CLOSED and no message, exits with ort::EXIT_START_FAILED.
auto start_next(launch::BootfsLauncher const &launcher, ours::Uandle const job,
                ours::Uandle const bootfs, ours::Uandle const vmex, ours::Uandle const vdso,
                ours::Uandle const debug, std::string_view const name, HandedOn const &environ)
    -> ours::Status {
    using ours::Status;

    // The child's root VMA first, filled in once the launcher has made the
    // child and handed it back; its handle_info is known already, so the
    // message can be encoded before.
    ours::Uandle handles[6] = {ours::Uandle::Invalid, job};
    u32 infos[6] = {ours::handle_info(ours::HandleType::VmaRoot, 0),
                    ours::handle_info(ours::HandleType::JobDefault, 0)};
    u32 count = 2;
    if (bootfs != ours::Uandle::Invalid) {
        handles[count] = bootfs;
        infos[count++] = ours::handle_info(ours::HandleType::VmoBootfs, 0);
    }
    // The vDSO's VMO: the next program starts processes too, and maps the
    // vDSO into them (K23).
    if (vdso != ours::Uandle::Invalid) {
        handles[count] = vdso;
        infos[count++] = ours::handle_info(ours::HandleType::VmoVdso, 0);
    }
    // Handed on after this program's own use of it, the BootFS's executable
    // copy: the next program makes its own (design D3).
    if (vmex != ours::Uandle::Invalid) {
        handles[count] = vmex;
        infos[count++] =
            ours::handle_info(ours::HandleType::SystemResource, u16(ours::RSRC_SYSTEM_VMEX_BASE));
    }
    if (debug != ours::Uandle::Invalid) {
        handles[count] = debug;
        infos[count++] =
            ours::handle_info(ours::HandleType::SystemResource, u16(ours::RSRC_SYSTEM_DEBUG_BASE));
    }

    // The header, six handle_infos, a name of at most 63 bytes and its NUL
    // -- the kernel keeps program names in 64 -- and the environment, a
    // subset of what the kernel relayed and so within USER_OPTIONS_BYTES:
    // the same sum the kernel sizes this program's own message by
    // (make_bootstrap()), so encoding cannot fail for want of room.
    constexpr usize MESSAGE_BYTES =
        sizeof(ours::ProcArgs) + 6 * sizeof(u32) + 64 + ours::USER_OPTIONS_BYTES;
    u8 message[MESSAGE_BYTES];
    u32 length = 0;
    std::string_view const args[] = {name};
    if (auto const status = ours::encode_process_args(
            message, {infos, count}, args, {environ.entries, environ.count}, {}, &length);
        status != Status::Ok) {
        return status;
    }

    ours::Uandle here = ours::Uandle::Invalid;
    ours::Uandle there = ours::Uandle::Invalid;
    if (auto const status = ours::channel_create(ours::ChannelCreateOptions::None, &here, &there);
        status != Status::Ok) {
        return status;
    }
    // |there| leaves this process whatever the launch answers.
    auto const started = launch::launch_from_bootfs(
        launcher, job, name, there, {.report = print_launch_error, .context = nullptr});
    if (started.is_err()) {
        auto _ = ours::handle_close(here);
        return started.unwrap_err();
    }
    handles[0] = started.unwrap().root_vma;
    // userboot does not wait for the next program: it exits once the message
    // is written. Closing the handle does not stop a started process.
    auto _ = ours::handle_close(started.unwrap().process);
    auto const written =
        ours::channel_write(here, ours::ChannelWriteOptions::None, message, length, handles, count);
    auto _ = ours::handle_close(here);
    return written;
}
} // namespace

auto main(int, char **) -> int {
    auto const next = ort::process_args().option(NEXT_OPTION);
    if (next.empty()) {
        // No program named: the proof-of-life image. Reaching this line at
        // all is what it proves -- the kernel built a process, mapped this
        // ELF and the vDSO into it, and the runtime read its processargs.
        auto _ = ort::debug_line("userboot: hello from ring 3");
        halt();
    }

    // From here on every path returns, and the runtime turns main's value
    // into process_exit: userboot's work is done once the next program has
    // what it needs, and a userboot that cannot give it that has nothing
    // else to do either. Zircon's userboot ends the same way on success and
    // on failure (main() in main.rs logs the error and returns). Its exit
    // takes nothing with it: the next program lives under the root job, not
    // under userboot, and holds the only handles userboot had.
    auto const job = ort::take_startup_handle(ours::HandleType::JobDefault, 0);
    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("userboot: started without the root job");
        return 1;
    }
    auto const bootfs = ort::take_startup_handle(ours::HandleType::VmoBootfs, 0);
    auto const vmex = ort::take_startup_handle(ours::HandleType::SystemResource,
                                               u16(ours::RSRC_SYSTEM_VMEX_BASE));
    auto const debug = ort::take_startup_handle(ours::HandleType::SystemResource,
                                                u16(ours::RSRC_SYSTEM_DEBUG_BASE));
    auto const vdso = ort::take_startup_handle(ours::HandleType::VmoVdso, 0);
    auto const vma = ort::take_startup_handle(ours::HandleType::VmaRoot, 0);

    // The launcher. Without the BootFS there is nothing to start the next
    // program from: the kernel loads no program but this one.
    if (bootfs == ours::Uandle::Invalid || vmex == ours::Uandle::Invalid ||
        vdso == ours::Uandle::Invalid || vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line(
            {"userboot: cannot start ", to_str(next), " (missing BootFS, vmex, vDSO or VmaRoot)"});
        return 1;
    }
    auto const made = launch::make_bootfs_launcher(
        bootfs, vmex, vdso, vma, {.report = print_launch_error, .context = nullptr});
    if (made.is_err()) {
        report("userboot: setting the launcher up = ", made.unwrap_err());
        return 1;
    }
    auto const &launcher = made.unwrap();

    auto _ = ort::debug_line({"userboot: starting ", to_str(next)});
    auto const handed_on = collect_handed_on();
    for (auto index = u32(0); index < handed_on.count; ++index) {
        auto _ = ort::debug_line(
            {"userboot: passing ", to_str(handed_on.entries[index]), " to ", to_str(next)});
    }
    auto const status = start_next(launcher, job, bootfs, vmex, vdso, debug, next, handed_on);
    // The executable copy was only for the launch; the next program makes
    // its own from the BootFS handed on.
    auto _ = ours::handle_close(launcher.executable);
    if (status != ours::Status::Ok) {
        report("userboot: starting the next program = ", status);
        return 1;
    }
    auto _ = ort::debug_line({"userboot: started ", to_str(next)});
    return 0;
}
