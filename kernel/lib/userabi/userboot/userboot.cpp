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

#include <string_view>

#include <ou/channel.hpp>
#include <ou/debug.hpp>
#include <ou/handle.hpp>
#include <ou/process.hpp>
#include <ou/str.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/types.hpp>
#include <runtime/main.hpp>
#include <runtime/startup.hpp>

namespace {
/// The option naming the program to start next. Only the `key=value` form
/// means anything, since a bare `userboot.next` names no program;
/// runtime::ProcessArgs::option() reads both as empty. Zircon's key of the
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
    for (auto const entry: runtime::process_args().environ()) {
        if (entry.starts_with(USERBOOT_OPTION_PREFIX)) {
            continue;
        }
        if (handed_on.count == ours::MAX_USER_OPTIONS) {
            static_cast<void>(
                ou::debug_line({"userboot: dropping ", to_str(entry), ": no room to hand it on"}));
            continue;
        }
        handed_on.entries[handed_on.count++] = entry;
    }
    return handed_on;
}

/// Starts |name| from the kernel package under |job|, handing it |job|,
/// |bootfs| and |vmex| as its JobDefault, VmoBootfs and SystemResource (slot
/// RSRC_SYSTEM_VMEX_BASE) startup handles -- exactly what the kernel handed
/// this program, so the next program starts as it would
/// have had the kernel started it directly. argv is {|name|}; the environment
/// is |environ|, the options that were not this program's.
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
                ours::HandleValue const vmex, ours::HandleValue const vdso,
                std::string_view const name, HandedOn const &environ) -> ours::Status {
    using ours::Status;

    // The child's root VMA first, filled in once process_spawn_boot has made
    // the child and handed it back; its handle_info is known already, so the
    // message can be encoded before.
    ours::HandleValue handles[5] = {ours::HANDLE_INVALID, job};
    u32 infos[5] = {ours::handle_info(ours::HandleType::VmaRoot, 0),
                    ours::handle_info(ours::HandleType::JobDefault, 0)};
    u32 count = 2;
    if (bootfs != ours::HANDLE_INVALID) {
        handles[count] = bootfs;
        infos[count++] = ours::handle_info(ours::HandleType::VmoBootfs, 0);
    }
    // The vDSO's VMO, handed on unused for now: it is the next program that
    // starts processes, and maps the vDSO into them (K23).
    if (vdso != ours::HANDLE_INVALID) {
        handles[count] = vdso;
        infos[count++] = ours::handle_info(ours::HandleType::VmoVdso, 0);
    }
    // Handed on unused: userboot has nothing to make executable before K19,
    // and the next program is the one design D3 gives it to.
    if (vmex != ours::HANDLE_INVALID) {
        handles[count] = vmex;
        infos[count++] =
            ours::handle_info(ours::HandleType::SystemResource, u16(ours::RSRC_SYSTEM_VMEX_BASE));
    }

    // The header, five handle_infos, a name of at most 63 bytes and its NUL
    // -- the kernel keeps program names in 64 -- and the environment, a
    // subset of what the kernel relayed and so within USER_OPTIONS_BYTES:
    // the same sum the kernel sizes this program's own message by
    // (make_bootstrap()), so encoding cannot fail for want of room.
    constexpr usize MESSAGE_BYTES =
        sizeof(ours::ProcArgs) + 5 * sizeof(u32) + 64 + ours::USER_OPTIONS_BYTES;
    u8 message[MESSAGE_BYTES];
    u32 length = 0;
    std::string_view const args[] = {name};
    if (auto const status = ours::encode_process_args(
            message, {infos, count}, args, {environ.entries, environ.count}, {}, &length);
        status != Status::Ok) {
        return status;
    }

    ours::HandleValue here = ours::HANDLE_INVALID;
    ours::HandleValue there = ours::HANDLE_INVALID;
    if (auto const status = ou::channel_create(&here, &there); status != Status::Ok) {
        return status;
    }
    if (auto const status = ou::process_spawn_boot(job, to_str(name), there, &handles[0]);
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
    auto const next = runtime::process_args().option(NEXT_OPTION);
    if (next.empty()) {
        // No program named: the proof-of-life image. Reaching this line at
        // all is what it proves -- the kernel built a process, mapped this
        // ELF and the vDSO into it, and the runtime read its processargs.
        static_cast<void>(ou::debug_line("userboot: hello from ring 3"));
        halt();
    }

    // From here on every path returns, and the runtime turns main's value
    // into process_exit: userboot's work is done once the next program has
    // what it needs, and a userboot that cannot give it that has nothing
    // else to do either. Zircon's userboot ends the same way on success and
    // on failure (main() in main.rs logs the error and returns). Its exit
    // takes nothing with it: the next program lives under the root job, not
    // under userboot, and holds the only handles userboot had.
    auto const job = runtime::take_startup_handle(ours::HandleType::JobDefault, 0);
    if (job == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("userboot: started without the root job"));
        return 1;
    }
    // Absent is allowed: the kernel starts this program without a BootFS
    // when it could not build one, and the next program is the one that has
    // to cope with that, as init already does.
    auto const bootfs = runtime::take_startup_handle(ours::HandleType::VmoBootfs, 0);
    auto const vmex = runtime::take_startup_handle(ours::HandleType::SystemResource,
                                                   u16(ours::RSRC_SYSTEM_VMEX_BASE));
    auto const vdso = runtime::take_startup_handle(ours::HandleType::VmoVdso, 0);

    static_cast<void>(ou::debug_line({"userboot: starting ", to_str(next)}));
    auto const handed_on = collect_handed_on();
    for (auto index = u32(0); index < handed_on.count; ++index) {
        static_cast<void>(ou::debug_line(
            {"userboot: passing ", to_str(handed_on.entries[index]), " to ", to_str(next)}));
    }
    if (auto const status = start_next(job, bootfs, vmex, vdso, next, handed_on);
        status != ours::Status::Ok) {
        report("userboot: starting the next program = ", status);
        return 1;
    }
    static_cast<void>(ou::debug_line({"userboot: started ", to_str(next)}));
    return 0;
}
