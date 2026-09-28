/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// A shell that runs in ring 3 and talks to the kernel only through the vDSO.
///
/// What it is for: userboot proved that the kernel can build a process, map an
/// ELF into it and answer one system call. This is the next question -- can a
/// *program*, one that is not part of the kernel's own bring-up, do something
/// useful with that. Listing the kshell command registry is the smallest
/// useful thing, and it is useful precisely because the registry lives on the
/// other side of the privilege boundary: the names come out of the kernel's
/// linker sections, and nothing in this file knows any of them.
///
/// Every system call it makes goes through //system/lib/ou, which is the one
/// place that knows what the raw ABI looks like. This file names what it wants
/// (ou::debug_line, ou::command_list) and knows nothing else -- not the
/// per-call size limit, not the symbols the loader binds, not that a trap is
/// involved at all.
///
/// It runs on the runtime (//system/lib/runtime): the entry point is the
/// runtime's, which reads the processargs message this program was started
/// with and calls main. What main finds among the startup handles says who
/// started it and what to do -- a channel to init (User0) to answer, the root
/// job to start fault probes under, the BootFS to look at. The C library it
/// links is mem* and strlen (//system/lib/libc), and check_libc() is where
/// this program makes sure it is there and right.

#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/vdso.hpp>

#include <string_view>

#include <cstring>

#include <bootfs/format.hpp>
#include <ou/channel.hpp>
#include <ou/command.hpp>
#include <ou/debug.hpp>
#include <ou/handle.hpp>
#include <ou/object.hpp>
#include <ou/process.hpp>
#include <ou/str.hpp>
#include <ou/vmo.hpp>
#include <runtime/main.hpp>
#include <runtime/startup.hpp>

namespace {
/// The kshell command registry, fetched into this program's own memory.
///
/// Sized rather than grown: there is no allocator in this image. NoSpace comes
/// back when the registry outgrows it, and the part that fits is still shown
/// -- a truncated list beats no list when the point is to see what exists.
class Registry {
  public:
    auto fetch() -> ours::Status {
        return ou::command_list(m_text);
    }

    [[nodiscard]]
    auto text() const -> ou::Str {
        return ou::command_text(m_text);
    }

  private:
    char m_text[2048] = {};
};

constexpr ou::Str BANNER{"ours shell: the kernel's command registry, seen from ring 3"};

/// Prints "<what><status name>" on a line of its own.
auto report(ou::Str const what, ours::Status const status) -> void {
    static_cast<void>(ou::debug_line({what, ou::Str::from_c_str(ours::to_string(status), 32)}));
}

/// Gives up a handle this program was started with, and checks that the
/// kernel then refuses it.
///
/// The first close has to succeed: the handle came from whoever started this
/// program, which sent it in the processargs message -- the root job when
/// userboot started it, a channel to init when init did -- and it entered
/// this process's table, under a value of this table's choosing, when the
/// runtime read that message before main. The second has to fail with
/// BadHandle, because the same number no longer names anything here -- and
/// that is the handle table's whole job, observed from the side of the
/// boundary that relies on it.
///
/// Closing init's channel is harmless to init, although init wakes on
/// PeerClosed as well as Readable: answer_init() runs first, so "pong" is
/// already queued on init's end, and a message stays readable after the
/// writer's end is gone. It is also what makes init print "shell closed its
/// channel", here rather than when this process ends.
auto check_handle_close(ours::HandleValue const startup) -> void {
    report("shell: close(startup handle) = ", ou::handle_close(startup));
    report("shell: close again = ", ou::handle_close(startup));
}

/// Prints "<what><got>" and whether that was |want|; answers whether it was.
auto expect(ou::Str const what, ours::Status const got, ours::Status const want) -> bool {
    auto const got_name = ou::Str::from_c_str(ours::to_string(got), 32);
    if (got == want) {
        static_cast<void>(ou::debug_line({what, got_name}));
        return true;
    }
    auto const want_name = ou::Str::from_c_str(ours::to_string(want), 32);
    static_cast<void>(ou::debug_line({what, got_name, " (expected ", want_name, ")"}));
    return false;
}

/// Whether the first |length| bytes of |bytes| spell |text|. By hand rather
/// than with memcmp(), so that check_libc() can check memcmp() and the rest
/// of the C library with something that does not depend on them.
auto spells(char const *const bytes, u32 const length, ou::Str const text) -> bool {
    if (length != text.size()) {
        return false;
    }
    for (u32 i = 0; i < length; ++i) {
        if (bytes[i] != text.data()[i]) {
            return false;
        }
    }
    return true;
}

/// Answers init, if init started this program.
///
/// Started by init, the User0 startup handle is a channel with "ping" on it
/// (or about to be): read it, print it, answer "pong" -- the other half of
/// the first slice, whose log lines are the completion criterion (design 5).
/// Started by userboot (dbg_1's `userboot.next=shell`) there is no such
/// handle, and nobody to answer.
auto answer_init(ours::HandleValue const peer) -> void {
    using ours::Signals;
    using ours::Status;

    if (peer == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: started by userboot; no one to answer"));
        return;
    }

    auto observed = Signals::None;
    auto status = ou::object_wait_one(peer, Signals::Readable | Signals::PeerClosed,
                                      ours::INSTANT_INFINITE, &observed);
    char buffer[32] = {};
    u32 actual = 0;
    if (status == Status::Ok) {
        status = ou::channel_read(peer, buffer, sizeof(buffer), &actual);
    }
    if (status != Status::Ok) {
        report("shell: reading init's channel = ", status);
        return;
    }
    static_cast<void>(ou::debug_line({"shell: init says \"", ou::Str(buffer, actual), "\""}));

    constexpr ou::Str PONG{"pong"};
    status = ou::channel_write(peer, PONG.data(), static_cast<u32>(PONG.size()));
    if (status != Status::Ok) {
        report("shell: answering = ", status);
    }
}

/// Reads the BootFS header through vmo_read and says how many files it
/// holds, as init does (the design's check for this step, K14). Only the
/// shell userboot starts has a BootFS; init keeps its own.
auto describe_bootfs(ours::HandleValue const bootfs) -> void {
    if (bootfs == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: started without a BootFS"));
        return;
    }
    bootfs::Header header{};
    if (auto const status = ou::vmo_read(bootfs, &header, 0, sizeof(header));
        status != ours::Status::Ok) {
        report("shell: reading the BootFS header = ", status);
        return;
    }
    if (header.magic != bootfs::Header::MAGIC) {
        static_cast<void>(ou::debug_line("shell: the BootFS header has the wrong magic"));
        return;
    }
    char count[10];
    static_cast<void>(
        ou::debug_line({"shell: bootfs has ", ou::decimal(header.file_count, count), " entries"}));
}

/// How one fault_probe run ended, from this side of its answer channel.
struct ProbeRun {
    /// Started at all; false when making the channels or the process failed,
    /// which has been reported already.
    bool started;
    /// Its end of the answer channel closed with nothing in it: the kernel
    /// killed it, or it never reached main.
    bool killed;
    char answer[32];
    u32 answer_size;
};

/// Starts fault_probe under |job| with |probe| as argv[1], and waits for it
/// to answer or die.
///
/// The probe is started the way every program on the runtime is: a
/// processargs message on its bootstrap channel (argv = {"fault_probe",
/// probe}), written before the process starts, with one startup handle --
/// the far end of a second channel, as User0, for the answer. The answer
/// cannot come back on the bootstrap channel, because the runtime closes that
/// as soon as it has read the message: waiting on it, this would see
/// PeerClosed before the probe had even reached main and count every probe
/// as killed.
auto run_probe(ours::HandleValue const job, ou::Str const probe) -> ProbeRun {
    using ours::Signals;
    using ours::Status;

    ProbeRun run{};

    // The message first, while this holds no handles: a name that does not
    // encode leaves nothing to clean up.
    u8 message[128];
    u32 length = 0;
    u32 const infos[] = {ours::handle_info(ours::HandleType::User0, 0)};
    std::string_view const args[] = {"fault_probe", {probe.data(), probe.size()}};
    if (auto const status = ours::encode_process_args(message, infos, args, {}, {}, &length);
        status != Status::Ok) {
        report("shell: encoding the probe's processargs = ", status);
        return run;
    }

    ours::HandleValue answer = ours::HANDLE_INVALID;
    ours::HandleValue answer_there = ours::HANDLE_INVALID;
    ours::HandleValue bootstrap = ours::HANDLE_INVALID;
    ours::HandleValue bootstrap_there = ours::HANDLE_INVALID;
    if (!expect("shell: probe channel_create = ", ou::channel_create(&answer, &answer_there),
                Status::Ok)) {
        return run;
    }
    if (!expect("shell: probe channel_create = ", ou::channel_create(&bootstrap, &bootstrap_there),
                Status::Ok)) {
        static_cast<void>(ou::handle_close(answer));
        static_cast<void>(ou::handle_close(answer_there));
        return run;
    }

    // |answer_there| leaves this process with the message, whatever the
    // write answers.
    auto status = ou::channel_write(bootstrap, message, length, &answer_there, 1);
    // The message is queued; this end has nothing more to say, and holding it
    // would keep a handle charged to this process for nothing.
    static_cast<void>(ou::handle_close(bootstrap));
    if (status != Status::Ok) {
        report("shell: writing the probe's processargs = ", status);
        static_cast<void>(ou::handle_close(answer));
        static_cast<void>(ou::handle_close(bootstrap_there));
        return run;
    }

    // |bootstrap_there| leaves this process whatever happens.
    if (status = ou::process_spawn_boot(job, "fault_probe", bootstrap_there);
        status != Status::Ok) {
        report("shell: spawning fault_probe = ", status);
        static_cast<void>(ou::handle_close(answer));
        return run;
    }
    run.started = true;

    auto observed = Signals::None;
    static_cast<void>(ou::object_wait_one(answer, Signals::Readable | Signals::PeerClosed,
                                          ours::INSTANT_INFINITE, &observed));
    run.killed = !(observed & Signals::Readable);
    if (!run.killed) {
        static_cast<void>(
            ou::channel_read(answer, run.answer, sizeof(run.answer), &run.answer_size));

        // And then until the probe has let go of its end. That end was made
        // here, so it is charged to this process (the per-creator handle
        // quota) for as long as it lives, and a probe that answered is still
        // on its way out: return now and the quota check further on counts
        // it -- one handle short, depending on how fast the probe dies. A
        // killed probe's end has closed already; that is what woke the wait.
        //
        // This narrows the window rather than closing it: the kernel raises
        // PEER_CLOSED in ~Handle before it refunds the handle to its creator
        // (docs/repairs/260926/object.md), so for a few instructions after
        // this wakes the handle may still be counted. The checks that run
        // before the quota check are what make that gap unobservable today.
        static_cast<void>(
            ou::object_wait_one(answer, Signals::PeerClosed, ours::INSTANT_INFINITE, &observed));
    }
    static_cast<void>(ou::handle_close(answer));
    return run;
}

/// Programs that fault, and the kernel still here afterwards (P1-A).
///
/// Each probe is a fresh fault_probe process (lib/syscall's test,
/// kernel/lib/syscall/test/fault_probe), told by its argv which fault to
/// commit (run_probe()). The kernel's answer to a fault from ring 3 is to
/// kill the process: its handle table goes with it, its end of the answer
/// channel closes, and this end sees PeerClosed with nothing to read. A
/// probe that survives writes back instead, and that is a failure. Between
/// the two, the kernel's log carries the report for each kill and the
/// destruction of the process and thread objects -- the parts of the chain
/// this program cannot see.
///
/// "killed" here only means the answer channel closed empty, which is also
/// what a probe that never reached main looks like. That each one ran is in
/// the log: a `fault_probe: <name>` line before each kill. And one more run,
/// with a name no probe has, is the control: it must answer "unknown" and
/// return from main, the one path through which the runtime ends a probe
/// with main's own value.
///
/// Run only by the shell userboot starts, which userboot hands the root job
/// a new process needs; the dev image starts the shell from init, with a
/// channel instead, and does not package fault_probe.
auto run_probes(ours::HandleValue const job) -> void {
    constexpr ou::Str PROBES[] = {
        "pf_unmapped", "pf_kernel", "pf_write_vdso",     "ud2",
        "gp_hlt",      "gp_int80",  "gp_int3",           "de",
        "db_tf",       "gp_io",     "trap_outside_vdso",
    };

    u32 killed = 0;
    u32 survived = 0;
    for (auto const probe: PROBES) {
        auto const run = run_probe(job, probe);
        if (!run.started) {
            return;
        }
        if (run.killed) {
            static_cast<void>(ou::debug_line({"shell: probe ", probe, " killed"}));
            ++killed;
        } else {
            static_cast<void>(ou::debug_line({"shell: probe ", probe, " SURVIVED (",
                                              ou::Str(run.answer, run.answer_size), ")"}));
            ++survived;
        }
    }

    char killed_text[10];
    char survived_text[10];
    static_cast<void>(
        ou::debug_line({"shell: probes done (", ou::decimal(killed, killed_text), " killed, ",
                        ou::decimal(survived, survived_text), " survived)"}));

    constexpr ou::Str CONTROL{"none"};
    auto const control = run_probe(job, CONTROL);
    if (!control.started) {
        return;
    }
    if (control.killed) {
        static_cast<void>(ou::debug_line(
            "shell: control probe none died without answering (expected \"unknown\")"));
    } else {
        static_cast<void>(ou::debug_line({"shell: control probe none answered \"",
                                          ou::Str(control.answer, control.answer_size), "\""}));
    }
}

/// A channel with both ends in this one process: everything the kernel
/// promises about one, checked from the side of the boundary that relies on
/// it. Nothing here blocks -- the waits are satisfied before they start (the
/// wait is level-triggered) -- so a single-threaded program can run it all.
/// A wait that really sleeps needs a second program to wake it.
auto check_channel_loopback() -> void {
    using ours::Signals;
    using ours::Status;

    auto ok = true;
    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (!expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        return;
    }

    constexpr ou::Str PING{"ping"};
    ok &= expect("shell: write(ping) = ",
                 ou::channel_write(a, PING.data(), static_cast<u32>(PING.size())), Status::Ok);

    // Already readable, so a poll must succeed and report Readable.
    auto observed = Signals::None;
    ok &= expect("shell: wait_one(READABLE, poll) = ",
                 ou::object_wait_one(b, Signals::Readable, 0, &observed), Status::Ok);
    ok &= !!(observed & Signals::Readable) && !!(observed & Signals::Writable);

    // Too small a buffer leaves the message where it is and says how long it
    // is.
    char buffer[16] = {};
    u32 actual = 0;
    ok &= expect("shell: read(2 bytes) = ", ou::channel_read(b, buffer, 2, &actual),
                 Status::BufferTooSmall);
    ok &= actual == PING.size();

    ok &=
        expect("shell: read = ", ou::channel_read(b, buffer, sizeof(buffer), &actual), Status::Ok);
    if (spells(buffer, actual, PING)) {
        static_cast<void>(ou::debug_line("shell: loopback ping ok"));
    } else {
        ok = false;
        static_cast<void>(ou::debug_line("shell: loopback read the wrong bytes"));
    }

    ok &= expect("shell: read on empty = ", ou::channel_read(b, buffer, sizeof(buffer), &actual),
                 Status::ShouldWait);

    // Closing one end tells the other: PeerClosed goes up, Writable comes
    // down, and every further read or write says so.
    ok &= expect("shell: close(a) = ", ou::handle_close(a), Status::Ok);
    ok &= expect("shell: wait_one(PEER_CLOSED) = ",
                 ou::object_wait_one(b, Signals::PeerClosed, ours::INSTANT_INFINITE, &observed),
                 Status::Ok);
    ok &= !!(observed & Signals::PeerClosed) && !(observed & Signals::Writable);
    ok &= expect("shell: read after peer closed = ",
                 ou::channel_read(b, buffer, sizeof(buffer), &actual), Status::PeerClosed);
    ok &= expect("shell: write after peer closed = ",
                 ou::channel_write(b, PING.data(), static_cast<u32>(PING.size())),
                 Status::PeerClosed);
    ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);

    if (ok) {
        static_cast<void>(ou::debug_line("shell: channel checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: channel checks FAILED"));
    }
}

/// Handles that travel in messages: out of the writer's table, into the
/// reader's, and consumed whatever the write answers.
///
/// Four things. A channel end sent through a channel still works on the other
/// side -- a message goes through it. A channel cannot be sent through
/// itself. A write with one bad value in its handle array fails, and still
/// takes the good handles with it: none is left behind for the caller to
/// wonder about. And a chain of channel ends, each queued in the next, a
/// thousand deep, can be dropped from the top: the kernel frees it as a loop
/// (Dispatcher::rc_recycle), not a thousand destructors deep on one stack.
///
/// Through ou, whose channel_read and channel_write carry handles.
auto check_handle_transfer() -> void {
    using ours::HandleValue;
    using ours::Status;

    constexpr ou::Str PING{"ping"};

    auto ok = true;
    HandleValue a = ours::HANDLE_INVALID;
    HandleValue b = ours::HANDLE_INVALID;
    HandleValue c = ours::HANDLE_INVALID;
    HandleValue d = ours::HANDLE_INVALID;
    if (!expect("shell: transfer channel_create(a, b) = ", ou::channel_create(&a, &b),
                Status::Ok) ||
        !expect("shell: transfer channel_create(c, d) = ", ou::channel_create(&c, &d),
                Status::Ok)) {
        return;
    }

    // d travels a -> b; what comes out of b is a new value for the same end.
    ok &= expect("shell: write(a, handles = [d]) = ", ou::channel_write(a, nullptr, 0, &d, 1),
                 Status::Ok);
    ok &= expect("shell: close(d) after sending it = ", ou::handle_close(d), Status::BadHandle);
    HandleValue got = ours::HANDLE_INVALID;
    u32 actual_bytes = 0;
    u32 actual_handles = 0;
    ok &= expect("shell: read(b) = ",
                 ou::channel_read(b, nullptr, 0, &actual_bytes, &got, 1, &actual_handles),
                 Status::Ok);
    ok &= actual_handles == 1 && got != ours::HANDLE_INVALID;
    ok &= expect("shell: write(c, ping) = ",
                 ou::channel_write(c, PING.data(), static_cast<u32>(PING.size())), Status::Ok);
    char buffer[8] = {};
    u32 length = 0;
    ok &= expect("shell: read(the received end) = ",
                 ou::channel_read(got, buffer, sizeof(buffer), &length), Status::Ok);
    ok &= spells(buffer, length, PING);

    // A channel sent through itself is refused -- and consumed like any
    // other handle in the array.
    ok &= expect("shell: write(a, handles = [a]) = ", ou::channel_write(a, nullptr, 0, &a, 1),
                 Status::NotSupported);
    ok &= expect("shell: close(a) after that = ", ou::handle_close(a), Status::BadHandle);

    // One bad value fails the write and takes the good handle anyway.
    HandleValue e = ours::HANDLE_INVALID;
    HandleValue f = ours::HANDLE_INVALID;
    ok &= expect("shell: transfer channel_create(e, f) = ", ou::channel_create(&e, &f), Status::Ok);
    HandleValue const mixed[] = {e, HandleValue{0x7ffffff3}};
    ok &=
        expect("shell: write(c, handles = [e, bad]) = ", ou::channel_write(c, nullptr, 0, mixed, 2),
               Status::BadHandle);
    ok &= expect("shell: close(e) after that = ", ou::handle_close(e), Status::BadHandle);

    // A thousand deep: each end queued in the next channel, then only the
    // last one kept -- and closed.
    constexpr u32 DEPTH = 1000;
    auto chain_ok = true;
    HandleValue inner = f;
    for (u32 i = 0; i < DEPTH && chain_ok; ++i) {
        HandleValue writer = ours::HANDLE_INVALID;
        HandleValue holder = ours::HANDLE_INVALID;
        chain_ok &= ou::channel_create(&writer, &holder) == Status::Ok;
        chain_ok &= ou::channel_write(writer, nullptr, 0, &inner, 1) == Status::Ok;
        chain_ok &= ou::handle_close(writer) == Status::Ok;
        inner = holder;
    }
    chain_ok &= ou::handle_close(inner) == Status::Ok;
    ok &= chain_ok;
    if (chain_ok) {
        static_cast<void>(ou::debug_line("shell: a 1000-deep chain of ends, closed"));
    } else {
        static_cast<void>(ou::debug_line("shell: the 1000-deep chain FAILED"));
    }

    ok &= ou::handle_close(b) == Status::Ok;
    ok &= ou::handle_close(c) == Status::Ok;
    ok &= ou::handle_close(got) == Status::Ok;

    if (ok) {
        static_cast<void>(ou::debug_line("shell: handle transfer checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: handle transfer checks FAILED"));
    }
}

/// The per-process handle quota (HANDLE_MAX_PER_PROCESS): channels are
/// created until the kernel answers NoResource, a pair is closed, and one more
/// create then works. Everything is closed again afterwards, so the rest of
/// the program runs with its quota back.
///
/// Only the number of pairs made is printed, not a fixed expectation: how many
/// handles this program already holds when it gets here depends on how it was
/// started.
auto check_handle_quota() -> void {
    using ours::HandleValue;
    using ours::Status;

    // One pair more than the quota could ever allow, so that the loop ends on
    // the kernel's refusal and never on running out of room here.
    constexpr u32 ROOM = ours::HANDLE_MAX_PER_PROCESS + 2;
    static HandleValue made[ROOM];
    u32 count = 0;
    auto status = Status::Ok;
    while (count + 2 <= ROOM) {
        status = ou::channel_create(&made[count], &made[count + 1]);
        if (status != Status::Ok) {
            break;
        }
        count += 2;
    }

    auto ok = expect("shell: channel_create past the quota = ", status, Status::NoResource);
    char count_text[10];
    static_cast<void>(ou::debug_line(
        {"shell: ", ou::decimal(count / 2, count_text), " channels made before the quota"}));

    if (count >= 2) {
        ok &= ou::handle_close(made[count - 1]) == Status::Ok;
        ok &= ou::handle_close(made[count - 2]) == Status::Ok;
        ok &= expect("shell: channel_create after closing one = ",
                     ou::channel_create(&made[count - 2], &made[count - 1]), Status::Ok);
    }
    for (u32 i = 0; i < count; ++i) {
        ok &= ou::handle_close(made[i]) == Status::Ok;
    }

    if (ok) {
        static_cast<void>(ou::debug_line("shell: handle quota checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: handle quota checks FAILED"));
    }
}

/// Two addresses no call may copy to or from, for the checks below. What each
/// one tests is in check_bad_pointers().
constexpr auto UNMAPPED = ours::VirtAddr{0x7fff'ffff'e000};
constexpr auto KERNEL = ours::VirtAddr{0xffff'8000'0000'0000};

/// Bad user pointers handed to the kernel: each call must come back InvArgs,
/// and the kernel must still be here to answer the next one.
///
/// The vDSO entry points are called directly rather than through ou, so that
/// the pointers reach the kernel exactly as written here. Three kinds:
///
///   UNMAPPED  inside the user half of the address space, so the range check
///             passes, but nothing is mapped there: the copy faults, the fault
///             handler cannot resolve it, and the copy's data-fault resume
///             turns it into InvArgs. Used as a source (copy_from_user) and as
///             a destination (copy_to_user), which are different copies.
///   KERNEL    the kernel half: refused by the range check before any copy.
///             It answered InvArgs before the resume existed and is here so
///             that it keeps doing so.
///   |vdso|    mapped, and in the user half, but read-only: the vDSO. Only
///             ever a destination -- reading it is legitimate -- and the copy
///             faults on a page that is present, so the fault is a protection
///             violation the address space refuses rather than a page it
///             cannot supply. Until every x86 leaf PTE stopped being built
///             writable whatever it asked for, these writes went through and
///             each call answered Ok, having written into the vDSO.
auto check_bad_pointers(ours::VirtAddr const vdso) -> void {
    using ours::Signals;
    using ours::Status;

    auto ok = true;
    ok &= expect("shell: debug_write(unmapped) = ",
                 ours_debug_write(reinterpret_cast<char const *>(UNMAPPED), 8), Status::InvArgs);
    ok &= expect("shell: debug_write(kernel half) = ",
                 ours_debug_write(reinterpret_cast<char const *>(KERNEL), 8), Status::InvArgs);

    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (!expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        return;
    }

    // The message is copied in before the channel is touched, so a bad source
    // leaves the channel as it was.
    ok &= expect("shell: channel_write(from unmapped) = ",
                 ours_channel_write(a, ours::ChannelWriteOptions::None,
                                    reinterpret_cast<void const *>(UNMAPPED), 8, nullptr, 0),
                 Status::InvArgs);
    constexpr char BYTE = 'x';
    ok &= expect("shell: channel_write(1 byte) = ",
                 ours_channel_write(a, ours::ChannelWriteOptions::None, &BYTE, 1, nullptr, 0),
                 Status::Ok);

    // Read into nothing: the message is taken off the queue before it is
    // copied out, so when the copy fails the message is gone -- the same
    // choice Zircon makes, and the next read finds the channel empty.
    u32 actual_bytes = 0;
    u32 actual_handles = 0;
    ok &= expect("shell: channel_read(into unmapped) = ",
                 ours_channel_read(b, ours::ChannelReadOptions::None,
                                   reinterpret_cast<void *>(UNMAPPED), nullptr, 1, 0, &actual_bytes,
                                   &actual_handles),
                 Status::InvArgs);
    char buffer[4] = {};
    ok &= expect("shell: channel_read(after the lost message) = ",
                 ours_channel_read(b, ours::ChannelReadOptions::None, buffer, nullptr,
                                   sizeof(buffer), 0, &actual_bytes, &actual_handles),
                 Status::ShouldWait);

    // A poll, so that the wait ends at once and the kernel goes on to write
    // |observed| -- into nothing.
    ok &=
        expect("shell: wait_one(observed = unmapped, poll) = ",
               ours_object_wait_one(b, Signals::Readable, 0, reinterpret_cast<Signals *>(UNMAPPED)),
               Status::InvArgs);

    // Both handle values are written out before either handle is installed,
    // so the failed create leaves nothing behind and the next one works.
    ours::HandleValue c = ours::HANDLE_INVALID;
    ours::HandleValue d = ours::HANDLE_INVALID;
    ok &= expect("shell: channel_create(out0 = unmapped) = ",
                 ours_channel_create(ours::ChannelCreateOptions::None,
                                     reinterpret_cast<ours::HandleValue *>(UNMAPPED), &d),
                 Status::InvArgs);
    ok &= expect("shell: channel_create(after that) = ",
                 ours_channel_create(ours::ChannelCreateOptions::None, &c, &d), Status::Ok);

    // The same three destinations, pointed at the vDSO: mapped, but not for
    // writing. The message is lost the same way, and nothing reaches the
    // vDSO -- the next call through it still works, which is the proof.
    ok &= expect("shell: channel_write(1 byte) = ",
                 ours_channel_write(a, ours::ChannelWriteOptions::None, &BYTE, 1, nullptr, 0),
                 Status::Ok);
    ok &=
        expect("shell: channel_read(into the vDSO) = ",
               ours_channel_read(b, ours::ChannelReadOptions::None, reinterpret_cast<void *>(vdso),
                                 nullptr, 1, 0, &actual_bytes, &actual_handles),
               Status::InvArgs);
    ok &= expect("shell: wait_one(observed = vDSO, poll) = ",
                 ours_object_wait_one(b, Signals::Readable, 0, reinterpret_cast<Signals *>(vdso)),
                 Status::InvArgs);
    ours::HandleValue e = ours::HANDLE_INVALID;
    ok &= expect("shell: channel_create(out0 = vDSO) = ",
                 ours_channel_create(ours::ChannelCreateOptions::None,
                                     reinterpret_cast<ours::HandleValue *>(vdso), &e),
                 Status::InvArgs);

    ok &= expect("shell: close(a) = ", ou::handle_close(a), Status::Ok);
    ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);
    ok &= expect("shell: close(c) = ", ou::handle_close(c), Status::Ok);
    ok &= expect("shell: close(d) = ", ou::handle_close(d), Status::Ok);

    if (ok) {
        static_cast<void>(ou::debug_line("shell: bad-pointer checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: bad-pointer checks FAILED"));
    }
}

/// Whether |a| and |b| hold the same |length| bytes, checked by hand for the
/// same reason as spells().
///
auto same_bytes(void const *const a, void const *const b, usize const length) -> bool {
    auto const *const x = static_cast<u8 const *>(a);
    auto const *const y = static_cast<u8 const *>(b);
    for (usize i = 0; i < length; ++i) {
        if (x[i] != y[i]) {
            return false;
        }
    }
    return true;
}

/// Big enough that copying one is a call to memcpy() rather than inline moves;
/// the compiler's inline limit for a copy is well under this.
struct Large {
    u8 bytes[256];
};

/// Makes |out| from |seed| through a pointer the compiler cannot see through,
/// so that neither the copy below nor the value copied can be folded away.
[[gnu::noinline]]
auto fill(Large *const out, u8 const seed) -> void {
    for (usize i = 0; i < sizeof(out->bytes); ++i) {
        out->bytes[i] = static_cast<u8>(seed + i);
    }
}

/// The C library every user program now links (//system/lib/libc): each
/// function once, with its answer checked by hand.
///
/// Two kinds of call, because both used to be link errors. Calls by name,
/// which under -ffreestanding stay calls whatever their arguments. And calls
/// the compiler makes on its own: a local array zero-initialised past its
/// inline limit is a memset(), a large struct copy a memcpy(). Before the
/// library was linked, writing either in a user program failed to link.
///
/// |seed| comes from outside (the vDSO's address), so that nothing below is
/// a constant the compiler could evaluate instead of calling.
auto check_libc(u8 const seed) -> void {
    auto ok = true;

    // By name, with a length the compiler does not know: the check the plan
    // asks for (design U1).
    char source[32] = {};
    for (usize i = 0; i < sizeof(source); ++i) {
        source[i] = static_cast<char>('a' + (seed + i) % 26);
    }
    char copy[32] = {};
    auto const length = static_cast<usize>(seed % 16) + 8;
    auto const *const returned = std::memcpy(copy, source, length);
    ok &= returned == copy && same_bytes(copy, source, length) && copy[length] == 0;

    // Overlap in both directions.
    char moving[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    std::memmove(moving + 2, moving, 8);
    ok &= spells(moving, 10, "0101234567");
    std::memmove(moving, moving + 2, 8);
    ok &= spells(moving, 10, "0123456767");

    // The matching byte itself, not the start of the buffer.
    ok &= std::memchr(moving, '6', 10) == moving + 6;
    ok &= std::memchr(moving, 'x', 10) == nullptr;

    // A NUL-terminated word of a length only known at run time.
    char word[16] = {};
    auto const letters = static_cast<usize>(seed % 8) + 3;
    for (usize i = 0; i < letters; ++i) {
        word[i] = 'w';
    }
    ok &= std::strlen(word) == letters;

    // Byte order as unsigned char: 0x80 sorts after 0x01.
    u8 const high[] = {0x80};
    u8 const low[] = {static_cast<u8>(seed % 2 + 1)};
    ok &= std::memcmp(high, low, 1) > 0 && std::memcmp(low, high, 1) < 0;
    ok &= std::memcmp(source, copy, length) == 0;

    // What the compiler calls on its own. |a| changes after the copy, so
    // |b| has to be bytes of its own: left unchanged, the compiler reads |a|
    // wherever |b| is used and drops the copy -- the call this is here for.
    Large a;
    fill(&a, seed);
    Large b = a;
    fill(&a, static_cast<u8>(seed + 1));
    Large expected;
    fill(&expected, seed);
    ok &= same_bytes(&b, &expected, sizeof(Large));
    u8 zeroed[256] = {};
    auto zero = true;
    for (auto const byte: zeroed) {
        zero &= byte == 0;
    }
    ok &= zero;

    if (ok) {
        static_cast<void>(ou::debug_line("shell: libc checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: libc checks FAILED"));
    }
}

/// vmo_read's refusals: what it says before it looks at a VMO.
///
/// Only what a call answers before it reads anything, so that this runs in
/// every image, with or without a BootFS; reads of a real VMO are made on the
/// BootFS, which only the shell userboot starts holds. What is checked here
/// is that the handle is resolved first and resolved strictly: a value that
/// names nothing is BadHandle, one that names something other than a VMO is
/// BadType, and both win over every argument error. So the calls below pair
/// a bad handle with a bad buffer (the kernel half), an offset no VMO could
/// reach, and a length of zero, and each must still be about the handle
/// (design 4.5: BadHandle > BadType > AccessDenied > the arguments).
auto check_vmo_read() -> void {
    using ours::Status;

    constexpr auto FAR = u64{0xffff'ffff'ffff'0000};

    auto ok = true;
    char buffer[16] = {};
    ok &= expect("shell: vmo_read(HANDLE_INVALID) = ",
                 ou::vmo_read(ours::HANDLE_INVALID, buffer, 0, sizeof(buffer)), Status::BadHandle);

    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (!expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        return;
    }
    ok &= expect("shell: vmo_read(a channel) = ", ou::vmo_read(a, buffer, 0, sizeof(buffer)),
                 Status::BadType);
    ok &= expect("shell: vmo_read(a channel, 0 bytes) = ", ou::vmo_read(a, buffer, 0, 0),
                 Status::BadType);
    ok &= expect("shell: vmo_read(a channel, into the kernel half) = ",
                 ou::vmo_read(a, reinterpret_cast<void *>(KERNEL), 0, sizeof(buffer)),
                 Status::BadType);
    ok &= expect("shell: vmo_read(a channel, far offset) = ",
                 ou::vmo_read(a, buffer, FAR, sizeof(buffer)), Status::BadType);

    ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);
    ok &= expect("shell: vmo_read(closed) = ", ou::vmo_read(b, buffer, 0, sizeof(buffer)),
                 Status::BadHandle);
    ok &= expect("shell: close(a) = ", ou::handle_close(a), Status::Ok);

    if (ok) {
        static_cast<void>(ou::debug_line("shell: vmo_read checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: vmo_read checks FAILED"));
    }
}

/// vmo_write's refusals: the handle is looked at before anything else.
///
/// The same order vmo_read keeps (design 4.5): BadHandle for a value that
/// names nothing, BadType for something that is not a VMO, and AccessDenied for
/// a VMO this handle may not write -- each ahead of every argument error, so the
/// calls below pair them with an empty write, a buffer in the kernel half and an
/// offset no VMO could reach. The BootFS handle is the VMO without Write: the
/// kernel mints it read-only. Its first bytes are read before and after, and a
/// refused write must have changed nothing.
///
/// A write that succeeds needs a VMO this program may write, and only
/// vmo_create makes one; those checks come with it.
auto check_vmo_write(ours::HandleValue const bootfs) -> void {
    using ours::Status;

    constexpr auto FAR = u64{0xffff'ffff'ffff'0000};

    auto ok = true;
    char const buffer[16] = {'o', 'v', 'e', 'r', 'w', 'r', 'i', 't', 't', 'e', 'n'};
    ok &= expect("shell: vmo_write(HANDLE_INVALID) = ",
                 ou::vmo_write(ours::HANDLE_INVALID, buffer, 0, sizeof(buffer)), Status::BadHandle);

    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (!expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        return;
    }
    ok &= expect("shell: vmo_write(a channel) = ", ou::vmo_write(a, buffer, 0, sizeof(buffer)),
                 Status::BadType);
    ok &= expect("shell: vmo_write(a channel, from the kernel half) = ",
                 ou::vmo_write(a, reinterpret_cast<void const *>(KERNEL), 0, sizeof(buffer)),
                 Status::BadType);
    ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);
    ok &= expect("shell: vmo_write(closed) = ", ou::vmo_write(b, buffer, 0, sizeof(buffer)),
                 Status::BadHandle);
    ok &= expect("shell: close(a) = ", ou::handle_close(a), Status::Ok);

    if (bootfs != ours::HANDLE_INVALID) {
        char before[sizeof(buffer)] = {};
        char after[sizeof(buffer)] = {};
        ok &= expect("shell: vmo_read(bootfs, before writing) = ",
                     ou::vmo_read(bootfs, before, 0, sizeof(before)), Status::Ok);
        ok &=
            expect("shell: vmo_write(bootfs) = ", ou::vmo_write(bootfs, buffer, 0, sizeof(buffer)),
                   Status::AccessDenied);
        ok &= expect("shell: vmo_write(bootfs, 0 bytes) = ", ou::vmo_write(bootfs, buffer, 0, 0),
                     Status::AccessDenied);
        ok &= expect(
            "shell: vmo_write(bootfs, from the kernel half, far offset) = ",
            ou::vmo_write(bootfs, reinterpret_cast<void const *>(KERNEL), FAR, sizeof(buffer)),
            Status::AccessDenied);
        ok &= expect("shell: vmo_read(bootfs, after writing) = ",
                     ou::vmo_read(bootfs, after, 0, sizeof(after)), Status::Ok);
        auto unchanged = true;
        for (usize i = 0; i < sizeof(before); ++i) {
            if (before[i] != after[i]) {
                unchanged = false;
            }
        }
        ok &= unchanged;
        if (unchanged) {
            static_cast<void>(ou::debug_line("shell: the BootFS is as it was"));
        } else {
            static_cast<void>(ou::debug_line("shell: the BootFS CHANGED"));
        }
    }

    if (ok) {
        static_cast<void>(ou::debug_line("shell: vmo_write checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: vmo_write checks FAILED"));
    }
}

/// vmo_create, and vmo_write on a VMO this program may write (K17).
///
/// Runs in every image: a VMO of its own is all it needs. What it pins down:
///   - the options: Resizable is known and NotSupported, any other bit InvArgs,
///     and an unknown bit wins over Resizable beside it;
///   - the size: rounded up to a page (a 3-page-and-a-byte VMO reads at its
///     last byte of page 4 and not one past it), zero is a valid empty VMO,
///     and a size no VMO can have is OutOfRange;
///   - lazy commit: a page nobody has written reads as zero;
///   - vmo_write: bytes written across a page boundary read back the same,
///     a range past the end is OutOfRange, and a handle without Write is
///     AccessDenied;
///   - a destination the handle value cannot be written to is InvArgs.
auto check_vmo_create() -> void {
    using ours::Rights;
    using ours::Status;
    using ours::VmoOptions;

    constexpr auto PAGE = u64{4096};

    auto ok = true;
    ours::HandleValue vmo = ours::HANDLE_INVALID;
    ok &=
        expect("shell: vmo_create(Resizable) = ", ou::vmo_create(PAGE, VmoOptions::Resizable, &vmo),
               Status::NotSupported);
    ok &= expect("shell: vmo_create(an unknown option) = ",
                 ou::vmo_create(PAGE, static_cast<VmoOptions>(1U << 5), &vmo), Status::InvArgs);
    ok &= expect("shell: vmo_create(Resizable and an unknown option) = ",
                 ou::vmo_create(PAGE, static_cast<VmoOptions>((1U << 1) | (1U << 5)), &vmo),
                 Status::InvArgs);
    ok &= expect("shell: vmo_create(the largest u64) = ",
                 ou::vmo_create(~u64{0}, VmoOptions::None, &vmo), Status::OutOfRange);
    ok &= expect(
        "shell: vmo_create(out = kernel half) = ",
        ou::vmo_create(PAGE, VmoOptions::None, reinterpret_cast<ours::HandleValue *>(KERNEL)),
        Status::InvArgs);

    // Empty: valid, and every non-empty range is past its end.
    ours::HandleValue empty = ours::HANDLE_INVALID;
    if (expect("shell: vmo_create(0 bytes) = ", ou::vmo_create(0, VmoOptions::None, &empty),
               Status::Ok)) {
        u8 byte = 0;
        ok &= expect("shell: vmo_read(empty, 0 bytes) = ", ou::vmo_read(empty, &byte, 0, 0),
                     Status::Ok);
        ok &= expect("shell: vmo_write(empty, 1 byte) = ", ou::vmo_write(empty, &byte, 0, 1),
                     Status::OutOfRange);
        ok &= expect("shell: close(empty) = ", ou::handle_close(empty), Status::Ok);
    } else {
        ok = false;
    }

    // 3 pages and a byte: rounded up to 4.
    if (!expect("shell: vmo_create(3 pages + 1 byte) = ",
                ou::vmo_create(3 * PAGE + 1, VmoOptions::None, &vmo), Status::Ok)) {
        static_cast<void>(ou::debug_line("shell: vmo_create checks FAILED"));
        return;
    }
    u8 tail[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    ok &= expect("shell: vmo_read(the last bytes of page 4) = ",
                 ou::vmo_read(vmo, tail, 4 * PAGE - sizeof(tail), sizeof(tail)), Status::Ok);
    auto zero = true;
    for (auto const b: tail) {
        if (b != 0) {
            zero = false;
        }
    }
    ok &= zero;
    if (zero) {
        static_cast<void>(ou::debug_line("shell: an unwritten page reads as zero"));
    }
    ok &= expect("shell: vmo_read(one byte past page 4) = ", ou::vmo_read(vmo, tail, 4 * PAGE, 1),
                 Status::OutOfRange);

    // A write across the first page boundary, read back.
    u8 const written[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    u8 back[16] = {};
    ok &= expect("shell: vmo_write(across a page boundary) = ",
                 ou::vmo_write(vmo, written, PAGE - 8, sizeof(written)), Status::Ok);
    ok &= expect("shell: vmo_read(it back) = ", ou::vmo_read(vmo, back, PAGE - 8, sizeof(back)),
                 Status::Ok);
    auto same = true;
    for (usize i = 0; i < sizeof(written); ++i) {
        if (written[i] != back[i]) {
            same = false;
        }
    }
    ok &= same;
    if (same) {
        static_cast<void>(ou::debug_line("shell: vmo_write reads back the same"));
    }
    ok &= expect("shell: vmo_write(offset = size, 1 byte) = ",
                 ou::vmo_write(vmo, written, 4 * PAGE, 1), Status::OutOfRange);
    ok &=
        expect("shell: vmo_write(size + 1 bytes) = ", ou::vmo_write(vmo, written, 0, 4 * PAGE + 1),
               Status::OutOfRange);
    ok &= expect("shell: vmo_write(from the kernel half) = ",
                 ou::vmo_write(vmo, reinterpret_cast<void const *>(KERNEL), 0, 8), Status::InvArgs);

    // A copy that may read but not write.
    ours::HandleValue readonly = ours::HANDLE_INVALID;
    if (expect("shell: handle_duplicate(vmo, without Write) = ",
               ou::handle_duplicate(vmo, Rights::Basic | Rights::Read | Rights::Map, &readonly),
               Status::Ok)) {
        ok &= expect("shell: vmo_write(without Write) = ",
                     ou::vmo_write(readonly, written, 0, sizeof(written)), Status::AccessDenied);
        ok &= expect("shell: close(readonly) = ", ou::handle_close(readonly), Status::Ok);
    } else {
        ok = false;
    }
    ok &= expect("shell: close(vmo) = ", ou::handle_close(vmo), Status::Ok);

    if (ok) {
        static_cast<void>(ou::debug_line("shell: vmo_create checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: vmo_create checks FAILED"));
    }
}

/// The BootFS the kernel built for user space, read through vmo_read: what is
/// in it, and what vmo_read does on a VMO that exists.
///
/// Two things were left to be checked from this side once a program held the
/// BootFS. That it holds no kernel file (design K12, the whitelist and the
/// blacklist behind it): every entry is printed with its size, and none may
/// be the kernel's image or code patches, whatever directory it sits in.
/// And the paths of vmo_read that need a real VMO (K13): a read across a
/// page boundary, the edges of the range, and destinations the kernel may
/// not write to.
///
/// Only the shell userboot starts holds the BootFS; started by init, this
/// says so and checks nothing.
auto check_bootfs(ours::HandleValue const bootfs, ours::VirtAddr const vdso) -> void {
    using ours::Status;

    if (bootfs == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: bootfs checks skipped (no BootFS here)"));
        return;
    }

    auto ok = true;
    bootfs::Header header{};
    ok &= expect("shell: vmo_read(header) = ", ou::vmo_read(bootfs, &header, 0, sizeof(header)),
                 Status::Ok);

    // The entry table and the names, into fixed buffers: this image's BootFS
    // has four files, and a much bigger one is not what this check is for.
    constexpr u32 MAX_ENTRIES = 16;
    constexpr u32 MAX_NAMES = 512;
    bootfs::Entry entries[MAX_ENTRIES];
    char names[MAX_NAMES];
    if (!ok || header.file_count == 0 || header.file_count > MAX_ENTRIES ||
        header.name_table_size > MAX_NAMES) {
        static_cast<void>(ou::debug_line("shell: bootfs checks FAILED (header)"));
        return;
    }
    auto const entries_size = usize{header.file_count} * sizeof(bootfs::Entry);
    ok &= expect("shell: vmo_read(entries) = ",
                 ou::vmo_read(bootfs, entries, sizeof(header), entries_size), Status::Ok);
    ok &= expect("shell: vmo_read(names) = ",
                 ou::vmo_read(bootfs, names, sizeof(header) + entries_size, header.name_table_size),
                 Status::Ok);

    for (u32 i = 0; ok && i < header.file_count; ++i) {
        auto const &entry = entries[i];
        if (u64{entry.name_offset} + entry.name_size > header.name_table_size) {
            ok = false;
            break;
        }
        auto const name = ou::Str(names + entry.name_offset, entry.name_size);
        char size_text[10];
        static_cast<void>(
            ou::debug_line({"shell: bootfs entry ", name, " (",
                            ou::decimal(static_cast<u32>(entry.data_size), size_text), " bytes)"}));

        // The kernel's own files, wherever they sit: the names
        // KernelPackage gives a kernel image and its patches.
        auto base = name;
        for (usize at = name.size(); at > 0; --at) {
            if (name.data()[at - 1] == '/') {
                base = ou::Str(name.data() + at, name.size() - at);
                break;
            }
        }
        if (spells(base.data(), static_cast<u32>(base.size()), "image.elf") ||
            spells(base.data(), static_cast<u32>(base.size()), "code-patches")) {
            static_cast<void>(ou::debug_line({"shell: bootfs holds a kernel file: ", name}));
            ok = false;
        }
    }

    // Across a page boundary: one read of 16 bytes that the kernel copies in
    // two steps, one per page, against two reads of 8 that each stay inside
    // a page. The boundary is inside one file's data, past its first page,
    // so both sides are that file's own bytes: at a file's start, every file
    // here would do -- they are all ELF programs and begin with the same
    // header -- and a second step that fetched the wrong file's first page
    // would still compare equal. The page size is the data alignment the
    // kernel records in every entry. And each buffer starts full of a
    // different pattern, so that a half nobody wrote cannot match the same
    // half of another.
    auto const page = entries[0].alignment;
    bootfs::Entry const *spanning = nullptr;
    for (u32 i = 0; i < header.file_count && spanning == nullptr; ++i) {
        if (entries[i].data_size >= page + 8) {
            spanning = &entries[i];
        }
    }
    if (spanning == nullptr) {
        static_cast<void>(ou::debug_line("shell: no BootFS file spans two pages"));
        ok = false;
    } else {
        auto const boundary = spanning->data_offset + page;
        u8 across[16];
        u8 before[8];
        u8 after[8];
        for (auto &b: across) {
            b = 0xa5;
        }
        for (auto &b: before) {
            b = 0x5a;
        }
        for (auto &b: after) {
            b = 0x3c;
        }
        ok &= expect("shell: vmo_read(across a page boundary) = ",
                     ou::vmo_read(bootfs, across, boundary - 8, sizeof(across)), Status::Ok);
        ok &= expect("shell: vmo_read(the page before) = ",
                     ou::vmo_read(bootfs, before, boundary - 8, sizeof(before)), Status::Ok);
        ok &= expect("shell: vmo_read(the page after) = ",
                     ou::vmo_read(bootfs, after, boundary, sizeof(after)), Status::Ok);
        ok &= same_bytes(across, before, sizeof(before)) &&
              same_bytes(across + sizeof(before), after, sizeof(after));
    }

    // The edges of the range: empty at the very end is in range, a byte
    // there is not, and neither is one byte more than the whole VMO.
    auto const size = header.total_size;
    u8 byte = 0;
    ok &= expect("shell: vmo_read(offset = size, 0 bytes) = ", ou::vmo_read(bootfs, &byte, size, 0),
                 Status::Ok);
    ok &= expect("shell: vmo_read(offset = size, 1 byte) = ", ou::vmo_read(bootfs, &byte, size, 1),
                 Status::OutOfRange);
    ok &= expect("shell: vmo_read(size + 1 bytes) = ",
                 ou::vmo_read(bootfs, &byte, 0, static_cast<usize>(size) + 1), Status::OutOfRange);

    // Destinations the kernel may not write: the kernel half (refused before
    // any page is read), nothing mapped, and the read-only vDSO (both found
    // while copying).
    ok &= expect("shell: vmo_read(into the kernel half) = ",
                 ou::vmo_read(bootfs, reinterpret_cast<void *>(KERNEL), 0, 8), Status::InvArgs);
    ok &= expect("shell: vmo_read(into unmapped) = ",
                 ou::vmo_read(bootfs, reinterpret_cast<void *>(UNMAPPED), 0, 8), Status::InvArgs);
    ok &= expect("shell: vmo_read(into the vDSO) = ",
                 ou::vmo_read(bootfs, reinterpret_cast<void *>(vdso), 0, 8), Status::InvArgs);

    if (ok) {
        static_cast<void>(ou::debug_line("shell: bootfs checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: bootfs checks FAILED"));
    }
}

/// handle_duplicate: a second handle to the same object, never with more
/// rights than the first.
///
/// What needs no particular object runs in every image: a value that names
/// nothing, and a channel, which is never made with Duplicate. What needs one
/// runs on the BootFS, which only the shell userboot starts holds: a copy
/// reads what the original reads; a right the original lacks, or SameRights
/// with a right beside it, is refused; a copy without Read cannot vmo_read;
/// and an out pointer the kernel cannot write gives no handle -- checked by
/// failing more times than the quota allows and then succeeding once.
auto check_handle_duplicate(ours::HandleValue const bootfs) -> void {
    using ours::Rights;
    using ours::Status;

    auto ok = true;
    ours::HandleValue copy = ours::HANDLE_INVALID;
    ok &= expect("shell: handle_duplicate(HANDLE_INVALID) = ",
                 ou::handle_duplicate(ours::HANDLE_INVALID, Rights::SameRights, &copy),
                 Status::BadHandle);

    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (!expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        return;
    }
    ok &= expect("shell: handle_duplicate(a channel) = ",
                 ou::handle_duplicate(a, Rights::SameRights, &copy), Status::AccessDenied);
    ok &= expect("shell: close(a) = ", ou::handle_close(a), Status::Ok);
    ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);

    if (bootfs == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: handle_duplicate on a VMO skipped (no BootFS)"));
    } else {
        // Same rights: both read the same header.
        ok &= expect("shell: handle_duplicate(bootfs, SameRights) = ",
                     ou::handle_duplicate(bootfs, Rights::SameRights, &copy), Status::Ok);
        bootfs::Header original{};
        bootfs::Header copied{};
        ok &= expect("shell: vmo_read(original) = ",
                     ou::vmo_read(bootfs, &original, 0, sizeof(original)), Status::Ok);
        ok &= expect("shell: vmo_read(copy) = ", ou::vmo_read(copy, &copied, 0, sizeof(copied)),
                     Status::Ok);
        ok &= copy != bootfs && same_bytes(&original, &copied, sizeof(original)) &&
              original.magic == bootfs::Header::MAGIC;
        ok &= expect("shell: close(copy) = ", ou::handle_close(copy), Status::Ok);

        // Never wider: the BootFS is handed out without Write, and bit 31
        // beside another right is not "the same rights".
        ok &= expect("shell: handle_duplicate(bootfs, Write) = ",
                     ou::handle_duplicate(bootfs, Rights::Write, &copy), Status::InvArgs);
        ok &= expect("shell: handle_duplicate(bootfs, SameRights | Read) = ",
                     ou::handle_duplicate(bootfs, Rights::SameRights | Rights::Read, &copy),
                     Status::InvArgs);

        // Narrower: without Read, the copy cannot be read through.
        ours::HandleValue unreadable = ours::HANDLE_INVALID;
        ok &= expect("shell: handle_duplicate(bootfs, without Read) = ",
                     ou::handle_duplicate(bootfs, Rights::Basic | Rights::Map, &unreadable),
                     Status::Ok);
        u8 byte = 0;
        ok &= expect("shell: vmo_read(the copy without Read) = ",
                     ou::vmo_read(unreadable, &byte, 0, 1), Status::AccessDenied);
        ok &= expect("shell: close(the copy without Read) = ", ou::handle_close(unreadable),
                     Status::Ok);

        // The out pointer is written before the handle is published: one the
        // kernel cannot write fails the call, and leaves no handle behind.
        // InvArgs alone would not show that -- a kernel that published first
        // and failed the copy afterwards answers the same, with the handle
        // left in the table. So fail one more time than this process may hold
        // handles, then succeed once: had any failure left its duplicate
        // behind, charged to this process, the quota would be used up by now
        // and the last call would answer NoResource.
        auto leaked = false;
        for (u32 i = 0; i < ours::HANDLE_MAX_PER_PROCESS + 1 && !leaked; ++i) {
            leaked = ours_handle_duplicate(bootfs, Rights::SameRights,
                                           reinterpret_cast<ours::HandleValue *>(KERNEL)) !=
                     Status::InvArgs;
        }
        ok &= expect("shell: handle_duplicate(out = kernel half), past the quota = ",
                     leaked ? Status::InternalError : Status::InvArgs, Status::InvArgs);
        ok &= expect("shell: handle_duplicate(bootfs) after that = ",
                     ou::handle_duplicate(bootfs, Rights::SameRights, &copy), Status::Ok);
        ok &= expect("shell: close(copy) = ", ou::handle_close(copy), Status::Ok);
    }

    if (ok) {
        static_cast<void>(ou::debug_line("shell: handle_duplicate checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: handle_duplicate checks FAILED"));
    }
}

/// vmo_replace_as_executable: vmex turns a VMO handle into one that may be
/// executed, and the handle it is given is consumed whatever the answer (K19).
///
/// What needs no vmex runs in every image, each on a VMO of its own so that
/// "the original is gone" can be checked after every call:
///   - vmex = HANDLE_INVALID is AccessDenied, and the VMO handle is gone;
///   - vmex a channel is BadType; vmex a value that names nothing, BadHandle;
///   - a VMO handle that is a channel is BadHandle, and the channel is gone.
/// What needs vmex runs where userboot started this program: the BootFS as
/// vmex is BadType (a VMO, not a resource), and the real vmex gives a handle
/// that reads what the original wrote, cannot be written through (Write is
/// dropped, design D3), and leaves the original's value naming nothing.
auto check_vmo_replace_as_executable(ours::HandleValue const bootfs, ours::HandleValue const vmex)
    -> void {
    using ours::Status;
    using ours::VmoOptions;

    constexpr auto PAGE = u64{4096};

    auto ok = true;
    ours::HandleValue exec = ours::HANDLE_INVALID;
    u8 byte = 0;

    // A fresh VMO for each refusal: every one of them consumes it.
    auto const refused = [&](ou::Str const what, ours::HandleValue const with, Status const want) {
        ours::HandleValue vmo = ours::HANDLE_INVALID;
        if (!expect("shell: vmo_create = ", ou::vmo_create(PAGE, VmoOptions::None, &vmo),
                    Status::Ok)) {
            return false;
        }
        auto passed = expect(what, ou::vmo_replace_as_executable(vmo, with, &exec), want);
        passed &= expect("shell: vmo_read(the consumed handle) = ", ou::vmo_read(vmo, &byte, 0, 1),
                         Status::BadHandle);
        return passed;
    };

    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (!expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        return;
    }
    ok &=
        refused("shell: vmo_replace_as_executable(vmex = HANDLE_INVALID) = ", ours::HANDLE_INVALID,
                Status::AccessDenied);
    ok &= refused("shell: vmo_replace_as_executable(vmex = a channel) = ", a, Status::BadType);

    // A value that names nothing: b's, once b is closed.
    auto const stale = b;
    ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);
    ok &= refused("shell: vmo_replace_as_executable(vmex = a closed handle) = ", stale,
                  Status::BadHandle);

    // Not a VMO: refused, and consumed all the same.
    ok &= expect("shell: vmo_replace_as_executable(vmo = a channel) = ",
                 ou::vmo_replace_as_executable(a, vmex, &exec), Status::BadHandle);
    ok &= expect("shell: close(the consumed channel) = ", ou::handle_close(a), Status::BadHandle);

    if (vmex == ours::HANDLE_INVALID || bootfs == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: vmo_replace_as_executable with vmex skipped"));
    } else {
        ok &= refused("shell: vmo_replace_as_executable(vmex = the BootFS) = ", bootfs,
                      Status::BadType);

        ours::HandleValue vmo = ours::HANDLE_INVALID;
        if (expect("shell: vmo_create = ", ou::vmo_create(PAGE, VmoOptions::None, &vmo),
                   Status::Ok)) {
            u8 const written = 0x5a;
            ok &= expect("shell: vmo_write(before the replace) = ",
                         ou::vmo_write(vmo, &written, 0, 1), Status::Ok);
            ok &= expect("shell: vmo_replace_as_executable(vmex) = ",
                         ou::vmo_replace_as_executable(vmo, vmex, &exec), Status::Ok);
            ok &= expect("shell: vmo_read(the original) = ", ou::vmo_read(vmo, &byte, 0, 1),
                         Status::BadHandle);
            ok &=
                expect("shell: vmo_read(the executable handle) = ", ou::vmo_read(exec, &byte, 0, 1),
                       Status::Ok);
            ok &= byte == written;
            ok &= expect("shell: vmo_write(the executable handle) = ",
                         ou::vmo_write(exec, &written, 0, 1), Status::AccessDenied);
            ok &= expect("shell: close(the executable handle) = ", ou::handle_close(exec),
                         Status::Ok);
        } else {
            ok = false;
        }
    }

    if (ok) {
        static_cast<void>(ou::debug_line("shell: vmo_replace_as_executable checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: vmo_replace_as_executable checks FAILED"));
    }
}
} // namespace

/// Finding the system calls is not this program's problem: the calls below
/// are undefined symbols that the kernel binds to the vDSO's definitions while
/// it loads this image (elfload::Binder).
auto main(int, char **) -> int {
    static_cast<void>(ou::debug_line(BANNER));

    // Started by init: a channel to answer on. Started by userboot: the root
    // job, to start probes under, and the BootFS. Which ones are there is how
    // this program knows who started it.
    auto const peer = runtime::take_startup_handle(ours::HandleType::User0, 0);
    auto const job = runtime::take_startup_handle(ours::HandleType::JobDefault, 0);
    auto const bootfs = runtime::take_startup_handle(ours::HandleType::VmoBootfs, 0);
    // vmex comes only from userboot (design D3): init keeps its own and hands
    // it to nobody, so in the image where init starts this program there is
    // none, and that is right.
    auto const vmex = runtime::take_startup_handle(ours::HandleType::SystemResource,
                                                   u16(ours::RSRC_SYSTEM_VMEX_BASE));
    if (vmex != ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: vmex resource ok"));
    } else {
        static_cast<void>(ou::debug_line("shell: started without vmex"));
    }

    answer_init(peer);
    describe_bootfs(bootfs);
    if (job != ours::HANDLE_INVALID) {
        run_probes(job);
    }
    // One startup handle closed twice: init's channel when there is one --
    // closing it is init's cue -- and otherwise the job, done with now.
    check_handle_close(peer != ours::HANDLE_INVALID ? peer : job);
    check_channel_loopback();
    check_handle_transfer();
    check_handle_quota();
    check_bad_pointers(runtime::vdso_base());
    check_vmo_read();
    check_vmo_write(bootfs);
    check_vmo_create();
    check_bootfs(bootfs, runtime::vdso_base());
    check_handle_duplicate(bootfs);
    check_vmo_replace_as_executable(bootfs, vmex);
    check_libc(static_cast<u8>(runtime::vdso_base() >> 20));

    static Registry registry;
    auto const status = registry.fetch();

    static_cast<void>(ou::debug_line("--- commands ---"));
    static_cast<void>(ou::debug_write(registry.text()));
    if (status == ours::Status::NoSpace) {
        static_cast<void>(
            ou::debug_line("(truncated: the registry outgrew this program's buffer)"));
    } else if (status != ours::Status::Ok) {
        static_cast<void>(ou::debug_line("(command_list failed)"));
    }
    static_cast<void>(ou::debug_line("--- end ---"));

    // Done: the runtime ends the process with this. Its remaining handles
    // close as it dies.
    return 0;
}
