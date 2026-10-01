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
#include <ou/thread.hpp>
#include <ou/vma.hpp>
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
/// probe}), written once the process exists -- its root VMA, which the spawn
/// hands back, is one of the two startup handles -- with the far end of a
/// second channel, as User0, for the answer. The answer
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
    u32 const infos[] = {ours::handle_info(ours::HandleType::User0, 0),
                         ours::handle_info(ours::HandleType::VmaRoot, 0)};
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

    // The process first: |bootstrap_there| leaves this process whatever
    // happens, and its root VMA comes back for the message.
    ours::HandleValue root_vma = ours::HANDLE_INVALID;
    auto status = ou::process_spawn_boot(job, "fault_probe", bootstrap_there, &root_vma);
    if (status != Status::Ok) {
        report("shell: spawning fault_probe = ", status);
        static_cast<void>(ou::handle_close(bootstrap));
        static_cast<void>(ou::handle_close(answer));
        static_cast<void>(ou::handle_close(answer_there));
        return run;
    }
    run.started = true;

    // Then the message. Both handles leave this process with it, whatever the
    // write answers; the probe waits for it, and if it never comes, sees this
    // end close and ends itself.
    ours::HandleValue handed[] = {answer_there, root_vma};
    status = ou::channel_write(bootstrap, message, length, handed, 2);
    // This end has nothing more to say, and holding it would keep a handle
    // charged to this process for nothing.
    static_cast<void>(ou::handle_close(bootstrap));
    if (status != Status::Ok) {
        report("shell: writing the probe's processargs = ", status);
    }

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

/// vmo_create_child(Slice): a window onto another VMO's pages (K20).
///
/// What needs no particular VMO runs in every image, on a Lazy VMO of its
/// own, which is the case a slice could only be trusted with once parent and
/// child shared one lock (VmCowPages::lock()):
///   - the window: bytes written through the parent at page 2 read back at
///     the slice's 0, and the other way round; one byte past the slice is
///     OutOfRange although the parent has it; a slice of a slice lands at the
///     sum of the offsets; a size of 1 rounds up to a page;
///   - the arguments: an unaligned offset, and a range past the parent's end,
///     are InvArgs; Snapshot, and Slice with NoWrite beside it, NotSupported;
///     an unknown bit InvArgs;
///   - the handle: a channel is BadType, one without Read AccessDenied.
/// What needs the BootFS runs where userboot started this program: a slice of
/// the `vdso` entry starts with the ELF magic, and inherits the BootFS
/// handle's lack of Write.
auto check_vmo_create_child(ours::HandleValue const bootfs) -> void {
    using ours::Rights;
    using ours::Status;
    using ours::VmoChildOptions;
    using ours::VmoOptions;

    constexpr auto PAGE = u64{4096};

    auto ok = true;
    ours::HandleValue parent = ours::HANDLE_INVALID;
    if (!expect("shell: vmo_create(3 pages) = ",
                ou::vmo_create(3 * PAGE, VmoOptions::None, &parent), Status::Ok)) {
        static_cast<void>(ou::debug_line("shell: vmo_create_child checks FAILED"));
        return;
    }

    // Parent to slice.
    u8 const down[4] = {0xd0, 0xd1, 0xd2, 0xd3};
    u8 got[4] = {};
    ok &= expect("shell: vmo_write(parent, page 2) = ",
                 ou::vmo_write(parent, down, PAGE, sizeof(down)), Status::Ok);
    ours::HandleValue slice = ours::HANDLE_INVALID;
    ok &= expect("shell: vmo_create_child(Slice, page 2) = ",
                 ou::vmo_create_child(parent, VmoChildOptions::Slice, PAGE, PAGE, &slice),
                 Status::Ok);
    ok &= expect("shell: vmo_read(slice, 0) = ", ou::vmo_read(slice, got, 0, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(down, got, sizeof(got));

    // Slice to parent: the slice commits the page the parent reads.
    u8 const up[4] = {0xa0, 0xa1, 0xa2, 0xa3};
    ok &= expect("shell: vmo_write(slice, 8) = ", ou::vmo_write(slice, up, 8, sizeof(up)),
                 Status::Ok);
    ok &= expect("shell: vmo_read(parent, page 2 + 8) = ",
                 ou::vmo_read(parent, got, PAGE + 8, sizeof(got)), Status::Ok);
    ok &= same_bytes(up, got, sizeof(got));

    // The window's end, which the parent's pages do not stop at.
    ok &= expect("shell: vmo_read(slice, one byte past its end) = ",
                 ou::vmo_read(slice, got, PAGE, 1), Status::OutOfRange);

    // A slice of a slice: the parent's pages 2..3, then page 1 of that.
    ours::HandleValue wide = ours::HANDLE_INVALID;
    ours::HandleValue nested = ours::HANDLE_INVALID;
    ok &= expect("shell: vmo_create_child(Slice, pages 2-3) = ",
                 ou::vmo_create_child(parent, VmoChildOptions::Slice, PAGE, 2 * PAGE, &wide),
                 Status::Ok);
    ok &= expect("shell: vmo_create_child(Slice of it, its page 1) = ",
                 ou::vmo_create_child(wide, VmoChildOptions::Slice, 0, 1, &nested), Status::Ok);
    ok &= expect("shell: vmo_read(nested, 8) = ", ou::vmo_read(nested, got, 8, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(up, got, sizeof(got));
    // A size of 1 was a page: its last byte is there, the next is not.
    ok &= expect("shell: vmo_read(nested, last byte of the page) = ",
                 ou::vmo_read(nested, got, PAGE - 1, 1), Status::Ok);
    ok &= expect("shell: vmo_read(nested, one past) = ", ou::vmo_read(nested, got, PAGE, 1),
                 Status::OutOfRange);
    ok &= expect("shell: close(nested) = ", ou::handle_close(nested), Status::Ok);
    ok &= expect("shell: close(wide) = ", ou::handle_close(wide), Status::Ok);

    // The arguments.
    ours::HandleValue refused = ours::HANDLE_INVALID;
    ok &= expect("shell: vmo_create_child(unaligned offset) = ",
                 ou::vmo_create_child(parent, VmoChildOptions::Slice, 8, PAGE, &refused),
                 Status::InvArgs);
    ok &= expect("shell: vmo_create_child(past the end) = ",
                 ou::vmo_create_child(parent, VmoChildOptions::Slice, 2 * PAGE, 2 * PAGE, &refused),
                 Status::InvArgs);
    ok &= expect("shell: vmo_create_child(SnapshotModified) = ",
                 ou::vmo_create_child(parent, VmoChildOptions::SnapshotModified, 0, PAGE, &refused),
                 Status::NotSupported);
    ok &= expect("shell: vmo_create_child(SnapshotFull | SnapshotOnWrite) = ",
                 ou::vmo_create_child(parent,
                                      static_cast<VmoChildOptions>(
                                          std::to_underlying(VmoChildOptions::SnapshotFull) |
                                          std::to_underlying(VmoChildOptions::SnapshotOnWrite)),
                                      0, PAGE, &refused),
                 Status::NotSupported);
    ok &= expect("shell: vmo_create_child(Slice | NoWrite) = ",
                 ou::vmo_create_child(
                     parent,
                     static_cast<VmoChildOptions>(std::to_underlying(VmoChildOptions::Slice) |
                                                  std::to_underlying(VmoChildOptions::NoWrite)),
                     0, PAGE, &refused),
                 Status::NotSupported);
    ok &= expect(
        "shell: vmo_create_child(an unknown bit) = ",
        ou::vmo_create_child(parent, static_cast<VmoChildOptions>(1U << 9), 0, PAGE, &refused),
        Status::InvArgs);

    // The handle.
    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        ok &= expect("shell: vmo_create_child(a channel) = ",
                     ou::vmo_create_child(a, VmoChildOptions::Slice, 0, PAGE, &refused),
                     Status::BadType);
        ok &= expect("shell: close(a) = ", ou::handle_close(a), Status::Ok);
        ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);
    } else {
        ok = false;
    }
    ours::HandleValue unreadable = ours::HANDLE_INVALID;
    ok &=
        expect("shell: handle_duplicate(parent, without Read) = ",
               ou::handle_duplicate(parent, Rights::Basic | Rights::Map, &unreadable), Status::Ok);
    ok &= expect("shell: vmo_create_child(without Read) = ",
                 ou::vmo_create_child(unreadable, VmoChildOptions::Slice, 0, PAGE, &refused),
                 Status::AccessDenied);
    ok &=
        expect("shell: close(the copy without Read) = ", ou::handle_close(unreadable), Status::Ok);

    // Snapshots: a copy-on-write clone. Neither side sees the other's writes after
    // it, and pages past the parent's end read as zero.
    ours::HandleValue source = ours::HANDLE_INVALID;
    ours::HandleValue full = ours::HANDLE_INVALID;
    ours::HandleValue on_write = ours::HANDLE_INVALID;
    u8 const before[4] = {0x5a, 0x5b, 0x5c, 0x5d};
    u8 const theirs[4] = {0x6a, 0x6b, 0x6c, 0x6d};
    u8 const mine[4] = {0x7a, 0x7b, 0x7c, 0x7d};
    u8 const zero[4] = {};
    ok &= expect("shell: vmo_create(snapshot source, 2 pages) = ",
                 ou::vmo_create(2 * PAGE, VmoOptions::None, &source), Status::Ok);
    ok &= expect("shell: vmo_write(source, 0) = ", ou::vmo_write(source, before, 0, sizeof(before)),
                 Status::Ok);
    ok &= expect("shell: vmo_create_child(SnapshotFull, 3 pages) = ",
                 ou::vmo_create_child(source, VmoChildOptions::SnapshotFull, 0, 3 * PAGE, &full),
                 Status::Ok);
    ok &= expect("shell: vmo_read(snapshot, 0) = ", ou::vmo_read(full, got, 0, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(before, got, sizeof(got));
    // The snapshot writes its page 0: the source still has its own.
    ok &= expect("shell: vmo_write(snapshot, 0) = ", ou::vmo_write(full, mine, 0, sizeof(mine)),
                 Status::Ok);
    ok &= expect("shell: vmo_read(source, 0) = ", ou::vmo_read(source, got, 0, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(before, got, sizeof(got));
    // The source writes its pages 0 and 1: the snapshot sees neither.
    ok &= expect("shell: vmo_write(source, 0) again = ",
                 ou::vmo_write(source, theirs, 0, sizeof(theirs)), Status::Ok);
    ok &= expect("shell: vmo_write(source, page 1) = ",
                 ou::vmo_write(source, theirs, PAGE, sizeof(theirs)), Status::Ok);
    ok &= expect("shell: vmo_read(snapshot, 0) again = ", ou::vmo_read(full, got, 0, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(mine, got, sizeof(got));
    ok &= expect("shell: vmo_read(snapshot, page 1) = ", ou::vmo_read(full, got, PAGE, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(zero, got, sizeof(got));
    // Past the source's end.
    ok &= expect("shell: vmo_read(snapshot, page 2) = ",
                 ou::vmo_read(full, got, 2 * PAGE, sizeof(got)), Status::Ok);
    ok &= same_bytes(zero, got, sizeof(got));
    // SnapshotOnWrite: a snapshot of the source as it is now.
    ok &= expect("shell: vmo_create_child(SnapshotOnWrite) = ",
                 ou::vmo_create_child(source, VmoChildOptions::SnapshotOnWrite, 0, PAGE, &on_write),
                 Status::Ok);
    ok &= expect("shell: vmo_read(on-write snapshot, 0) = ",
                 ou::vmo_read(on_write, got, 0, sizeof(got)), Status::Ok);
    ok &= same_bytes(theirs, got, sizeof(got));
    ok &= expect("shell: vmo_create_child(SnapshotFull, unaligned offset) = ",
                 ou::vmo_create_child(source, VmoChildOptions::SnapshotFull, 8, PAGE, &refused),
                 Status::InvArgs);
    // A snapshot of a handle without Write can be written, and what it writes
    // stays its own: unlike a slice, it has no way to the source's pages.
    ours::HandleValue readonly = ours::HANDLE_INVALID;
    ours::HandleValue writable = ours::HANDLE_INVALID;
    ok &=
        expect("shell: handle_duplicate(source, without Write) = ",
               ou::handle_duplicate(source, Rights::Basic | Rights::Read | Rights::Map, &readonly),
               Status::Ok);
    ok &= expect("shell: vmo_write(source, through the read-only copy) = ",
                 ou::vmo_write(readonly, mine, 0, sizeof(mine)), Status::AccessDenied);
    ok &= expect("shell: vmo_create_child(the read-only copy, SnapshotFull) = ",
                 ou::vmo_create_child(readonly, VmoChildOptions::SnapshotFull, 0, PAGE, &writable),
                 Status::Ok);
    ok &= expect("shell: vmo_write(a snapshot of the read-only copy) = ",
                 ou::vmo_write(writable, mine, 0, sizeof(mine)), Status::Ok);
    ok &=
        expect("shell: vmo_read(source, 0) after it = ", ou::vmo_read(source, got, 0, sizeof(got)),
               Status::Ok);
    ok &= same_bytes(theirs, got, sizeof(got));
    ok &= expect("shell: close(the writable snapshot) = ", ou::handle_close(writable), Status::Ok);
    ok &= expect("shell: close(the read-only copy) = ", ou::handle_close(readonly), Status::Ok);
    ok &= expect("shell: close(on-write snapshot) = ", ou::handle_close(on_write), Status::Ok);
    ok &= expect("shell: close(snapshot) = ", ou::handle_close(full), Status::Ok);
    ok &= expect("shell: close(snapshot source) = ", ou::handle_close(source), Status::Ok);

    ok &= expect("shell: close(slice) = ", ou::handle_close(slice), Status::Ok);
    ok &= expect("shell: close(parent) = ", ou::handle_close(parent), Status::Ok);

    if (bootfs == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: vmo_create_child on the BootFS skipped"));
    } else {
        // The `vdso` entry, found the way check_bootfs() reads the directory.
        bootfs::Header header{};
        constexpr u32 MAX_ENTRIES = 16;
        constexpr u32 MAX_NAMES = 512;
        bootfs::Entry entries[MAX_ENTRIES];
        char names[MAX_NAMES];
        auto found = false;
        ok &= expect("shell: vmo_read(header) = ", ou::vmo_read(bootfs, &header, 0, sizeof(header)),
                     Status::Ok);
        if (header.file_count <= MAX_ENTRIES && header.name_table_size <= MAX_NAMES) {
            auto const entries_size = usize{header.file_count} * sizeof(bootfs::Entry);
            ok &= expect("shell: vmo_read(entries) = ",
                         ou::vmo_read(bootfs, entries, sizeof(header), entries_size), Status::Ok);
            ok &= expect(
                "shell: vmo_read(names) = ",
                ou::vmo_read(bootfs, names, sizeof(header) + entries_size, header.name_table_size),
                Status::Ok);
            for (u32 i = 0; ok && !found && i < header.file_count; ++i) {
                auto const &entry = entries[i];
                if (u64{entry.name_offset} + entry.name_size > header.name_table_size ||
                    !spells(names + entry.name_offset, entry.name_size, "vdso")) {
                    continue;
                }
                found = true;
                ours::HandleValue vdso = ours::HANDLE_INVALID;
                ok &= expect("shell: vmo_create_child(bootfs, Slice, vdso) = ",
                             ou::vmo_create_child(bootfs, VmoChildOptions::Slice, entry.data_offset,
                                                  entry.data_size, &vdso),
                             Status::Ok);
                char magic[4] = {};
                ok &= expect("shell: vmo_read(the vdso slice, 0) = ",
                             ou::vmo_read(vdso, magic, 0, sizeof(magic)), Status::Ok);
                ok &= spells(magic, sizeof(magic),
                             "\x7f"
                             "ELF");
                ok &=
                    expect("shell: vmo_write(the vdso slice) = ", ou::vmo_write(vdso, magic, 0, 1),
                           Status::AccessDenied);
                ok &= expect("shell: close(the vdso slice) = ", ou::handle_close(vdso), Status::Ok);
            }
        }
        if (!found) {
            static_cast<void>(ou::debug_line("shell: no vdso entry in the BootFS"));
            ok = false;
        }
    }

    if (ok) {
        static_cast<void>(ou::debug_line("shell: vmo_create_child checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: vmo_create_child checks FAILED"));
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

/// vma_map through the root VMA the kernel hands every process (K21).
///
/// Runs in every image, on VMOs of its own:
///   - a mapping reads what the VMO holds, from vmo_offset on, and a write
///     through a writable one is what vmo_read then finds;
///   - a slice maps like any VMO, and a mapping longer than it is OutOfRange;
///   - the protection: Write or Execute without Read, and Write with
///     Execute, are InvArgs, and so is a CanMap bit a program passes;
///   - the rights: a VMO handle without Map, and Execute on a VMO that has
///     none (vmo_create never gives it), are AccessDenied;
///   - the arguments: length 0, an unaligned vmo_offset, and a vma_offset
///     without Specific are InvArgs; a range past the VMO is OutOfRange; an
///     option named but not acted on (Compact) is NotSupported;
///   - an address that cannot be written back takes the mapping down: the
///     same Specific spot maps fine on the next try.
/// Where userboot started this program it also maps an executable copy of
/// the BootFS (vmo_replace_as_executable) read and execute, and the BootFS
/// itself, which has no Write, read-write is AccessDenied.
auto check_vma_map(ours::HandleValue const vma, ours::HandleValue const bootfs,
                   ours::HandleValue const vmex) -> void {
    using ours::Rights;
    using ours::Status;
    using ours::VmoOptions;
    using ours::VmOptions;

    constexpr auto PAGE = u64{4096};
    constexpr auto R = VmOptions::PermRead;
    constexpr auto RW = VmOptions::PermRead | VmOptions::PermWrite;
    constexpr auto RX = VmOptions::PermRead | VmOptions::PermExecute;

    if (vma == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: vma_map checks FAILED (no VmaRoot)"));
        return;
    }

    auto ok = true;
    ours::HandleValue vmo = ours::HANDLE_INVALID;
    if (!expect("shell: vmo_create(2 pages) = ", ou::vmo_create(2 * PAGE, VmoOptions::None, &vmo),
                Status::Ok)) {
        static_cast<void>(ou::debug_line("shell: vma_map checks FAILED"));
        return;
    }
    u8 const second[4] = {0x21, 0x22, 0x23, 0x24};
    ok &= expect("shell: vmo_write(page 2) = ", ou::vmo_write(vmo, second, PAGE, sizeof(second)),
                 Status::Ok);

    // Read-only, from vmo_offset = one page: the mapping starts at page 2.
    ours::VirtAddr at = 0;
    ok &= expect("shell: vma_map(R, vmo_offset = page 2) = ",
                 ou::vma_map(vma, R, 0, vmo, PAGE, PAGE, &at), Status::Ok);
    if (at != 0) {
        auto const *const bytes = reinterpret_cast<u8 const volatile *>(at);
        u8 got[4] = {bytes[0], bytes[1], bytes[2], bytes[3]};
        ok &= same_bytes(second, got, sizeof(got));
    }

    // Read-write: a store through the mapping is what vmo_read finds.
    ours::VirtAddr rw = 0;
    ok &= expect("shell: vma_map(RW) = ", ou::vma_map(vma, RW, 0, vmo, 0, PAGE, &rw), Status::Ok);
    if (rw != 0) {
        *reinterpret_cast<u8 volatile *>(rw + 16) = 0x77;
        u8 back = 0;
        ok &= expect("shell: vmo_read(what the mapping wrote) = ", ou::vmo_read(vmo, &back, 16, 1),
                     Status::Ok);
        ok &= back == 0x77;
    }

    // A slice maps like any VMO, and no further than its own end.
    ours::HandleValue slice = ours::HANDLE_INVALID;
    ok &= expect("shell: vmo_create_child(Slice, page 2) = ",
                 ou::vmo_create_child(vmo, ours::VmoChildOptions::Slice, PAGE, PAGE, &slice),
                 Status::Ok);
    ours::VirtAddr sliced = 0;
    ok &= expect("shell: vma_map(R, the slice) = ", ou::vma_map(vma, R, 0, slice, 0, PAGE, &sliced),
                 Status::Ok);
    if (sliced != 0) {
        ok &= *reinterpret_cast<u8 const volatile *>(sliced) == second[0];
    }
    ok &= expect("shell: vma_map(R, 2 pages of a 1-page slice) = ",
                 ou::vma_map(vma, R, 0, slice, 0, 2 * PAGE, &sliced), Status::OutOfRange);
    ok &= expect("shell: close(slice) = ", ou::handle_close(slice), Status::Ok);

    // The protection.
    ours::VirtAddr refused = 0;
    ok &=
        expect("shell: vma_map(W without R) = ",
               ou::vma_map(vma, VmOptions::PermWrite, 0, vmo, 0, PAGE, &refused), Status::InvArgs);
    ok &= expect("shell: vma_map(X without R) = ",
                 ou::vma_map(vma, VmOptions::PermExecute, 0, vmo, 0, PAGE, &refused),
                 Status::InvArgs);
    ok &= expect("shell: vma_map(RWX) = ",
                 ou::vma_map(vma, RW | VmOptions::PermExecute, 0, vmo, 0, PAGE, &refused),
                 Status::InvArgs);
    ok &= expect("shell: vma_map(R | CanMapWrite) = ",
                 ou::vma_map(vma, R | VmOptions::CanMapWrite, 0, vmo, 0, PAGE, &refused),
                 Status::InvArgs);

    // The rights.
    ok &= expect("shell: vma_map(RX, a VMO without Execute) = ",
                 ou::vma_map(vma, RX, 0, vmo, 0, PAGE, &refused), Status::AccessDenied);
    ours::HandleValue unmappable = ours::HANDLE_INVALID;
    ok &= expect("shell: handle_duplicate(vmo, without Map) = ",
                 ou::handle_duplicate(vmo, Rights::Basic | Rights::Read, &unmappable), Status::Ok);
    ok &= expect("shell: vma_map(R, a handle without Map) = ",
                 ou::vma_map(vma, R, 0, unmappable, 0, PAGE, &refused), Status::AccessDenied);
    ok &= expect("shell: close(the copy without Map) = ", ou::handle_close(unmappable), Status::Ok);

    // The arguments.
    ok &= expect("shell: vma_map(length 0) = ", ou::vma_map(vma, R, 0, vmo, 0, 0, &refused),
                 Status::InvArgs);
    ok &= expect("shell: vma_map(unaligned vmo_offset) = ",
                 ou::vma_map(vma, R, 0, vmo, 8, PAGE, &refused), Status::InvArgs);
    ok &= expect("shell: vma_map(vma_offset without Specific) = ",
                 ou::vma_map(vma, R, PAGE, vmo, 0, PAGE, &refused), Status::InvArgs);
    ok &= expect("shell: vma_map(past the VMO) = ",
                 ou::vma_map(vma, R, 0, vmo, PAGE, 2 * PAGE, &refused), Status::OutOfRange);
    ok &= expect("shell: vma_map(Compact) = ",
                 ou::vma_map(vma, R | VmOptions::Compact, 0, vmo, 0, PAGE, &refused),
                 Status::NotSupported);

    // An address the kernel cannot write back: the mapping is taken down, so
    // the same fixed spot is free for the next call. Had it stayed, that call
    // would find the spot taken.
    constexpr auto SPOT = usize{1} << 30;
    ok &= expect("shell: vma_map(Specific, mapped_addr = kernel half) = ",
                 ou::vma_map(vma, R | VmOptions::Specific, SPOT, vmo, 0, PAGE,
                             reinterpret_cast<ours::VirtAddr *>(KERNEL)),
                 Status::InvArgs);
    ok &=
        expect("shell: vma_map(Specific, the same spot) = ",
               ou::vma_map(vma, R | VmOptions::Specific, SPOT, vmo, 0, PAGE, &refused), Status::Ok);

    // Handles of the wrong kind.
    ok &= expect("shell: vma_map(vma = a VMO) = ", ou::vma_map(vmo, R, 0, vmo, 0, PAGE, &refused),
                 Status::BadType);
    ok &= expect("shell: close(vmo) = ", ou::handle_close(vmo), Status::Ok);

    if (bootfs == ours::HANDLE_INVALID || vmex == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: vma_map of the BootFS skipped"));
    } else {
        ok &= expect("shell: vma_map(RW, the BootFS) = ",
                     ou::vma_map(vma, RW, 0, bootfs, 0, PAGE, &refused), Status::AccessDenied);

        ours::HandleValue copy = ours::HANDLE_INVALID;
        ours::HandleValue executable = ours::HANDLE_INVALID;
        ok &= expect("shell: handle_duplicate(bootfs) = ",
                     ou::handle_duplicate(bootfs, Rights::SameRights, &copy), Status::Ok);
        ok &= expect("shell: vmo_replace_as_executable(the copy) = ",
                     ou::vmo_replace_as_executable(copy, vmex, &executable), Status::Ok);
        ours::VirtAddr code = 0;
        ok &= expect("shell: vma_map(RX, the executable BootFS) = ",
                     ou::vma_map(vma, RX, 0, executable, 0, PAGE, &code), Status::Ok);
        if (code != 0) {
            bootfs::Header header{};
            auto const *const bytes = reinterpret_cast<u8 const volatile *>(code);
            auto *const out = reinterpret_cast<u8 *>(&header);
            for (usize i = 0; i < sizeof(header); ++i) {
                out[i] = bytes[i];
            }
            ok &= header.magic == bootfs::Header::MAGIC;
        }
        ok &= expect("shell: close(the executable BootFS) = ", ou::handle_close(executable),
                     Status::Ok);
    }

    if (vmex == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: vma_map of an executable snapshot skipped"));
    } else {
        // A snapshot of an executable VMO: Write added, Execute taken away. It maps
        // read-write and not executable, and a store through that mapping takes a
        // copy of the page, which the executable VMO's own mapping never sees.
        ours::HandleValue plain = ours::HANDLE_INVALID;
        ours::HandleValue executable = ours::HANDLE_INVALID;
        ours::HandleValue snapshot = ours::HANDLE_INVALID;
        u8 const first[1] = {0x5a};
        ok &= expect("shell: vmo_create(to be executable) = ",
                     ou::vmo_create(PAGE, VmoOptions::None, &plain), Status::Ok);
        ok &= expect("shell: vmo_write(to be executable) = ",
                     ou::vmo_write(plain, first, 0, sizeof(first)), Status::Ok);
        ok &= expect("shell: vmo_replace_as_executable(it) = ",
                     ou::vmo_replace_as_executable(plain, vmex, &executable), Status::Ok);
        ours::VirtAddr code = 0;
        ok &= expect("shell: vma_map(RX, the executable VMO) = ",
                     ou::vma_map(vma, RX, 0, executable, 0, PAGE, &code), Status::Ok);
        ok &= expect("shell: vmo_create_child(the executable VMO, SnapshotFull) = ",
                     ou::vmo_create_child(executable, ours::VmoChildOptions::SnapshotFull, 0, PAGE,
                                          &snapshot),
                     Status::Ok);
        ok &= expect("shell: vma_map(RX, its snapshot) = ",
                     ou::vma_map(vma, RX, 0, snapshot, 0, PAGE, &refused), Status::AccessDenied);
        ours::VirtAddr data = 0;
        ok &= expect("shell: vma_map(RW, its snapshot) = ",
                     ou::vma_map(vma, RW, 0, snapshot, 0, PAGE, &data), Status::Ok);
        if (data != 0 && code != 0) {
            auto *const mine = reinterpret_cast<u8 volatile *>(data);
            auto const *const theirs = reinterpret_cast<u8 const volatile *>(code);
            ok &= mine[0] == first[0];
            mine[0] = 0xa5;
            ok &= mine[0] == 0xa5;
            ok &= theirs[0] == first[0];
            if (mine[0] != 0xa5 || theirs[0] != first[0]) {
                static_cast<void>(ou::debug_line("shell: a store through the snapshot's mapping "
                                                 "reached the executable VMO, or did not land"));
            }
        }
        ok &= expect("shell: close(the snapshot) = ", ou::handle_close(snapshot), Status::Ok);
        ok &=
            expect("shell: close(the executable VMO) = ", ou::handle_close(executable), Status::Ok);
    }

    if (ok) {
        static_cast<void>(ou::debug_line("shell: vma_map checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: vma_map checks FAILED"));
    }
}

/// vma_allocate: a guard, a range of the root VMA that nothing can be
/// mapped into (K22).
///
/// Runs in every image. What it pins down:
///   - the guard holds its range: a Specific mapping over it is refused, and
///     still is after the guard's handle is closed -- the range belongs to
///     the parent region, not to the handle;
///   - the guard itself allows nothing: mapping into it is AccessDenied, its
///     handle having none of Read, Write or Execute;
///   - the options: only Specific is taken today, so CanMapRead and a missing
///     Specific are NotSupported, and a Perm bit is InvArgs;
///   - the arguments: a size of 0 and an unaligned offset are InvArgs;
///   - a value the kernel cannot write back releases the range: the same
///     spot can be reserved on the next call.
auto check_vma_allocate(ours::HandleValue const vma) -> void {
    using ours::Status;
    using ours::VmoOptions;
    using ours::VmOptions;

    constexpr auto PAGE = u64{4096};
    // Past check_vma_map()'s fixed spot, and as far from anything else.
    constexpr auto SPOT = (usize{1} << 30) + 64 * PAGE;
    constexpr auto SPECIFIC_R = VmOptions::PermRead | VmOptions::Specific;

    if (vma == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: vma_allocate checks FAILED (no VmaRoot)"));
        return;
    }

    auto ok = true;
    ours::HandleValue vmo = ours::HANDLE_INVALID;
    if (!expect("shell: vmo_create = ", ou::vmo_create(PAGE, VmoOptions::None, &vmo), Status::Ok)) {
        static_cast<void>(ou::debug_line("shell: vma_allocate checks FAILED"));
        return;
    }

    ours::HandleValue guard = ours::HANDLE_INVALID;
    ours::VirtAddr at = 0;
    ok &= expect("shell: vma_allocate(Specific, 1 page) = ",
                 ou::vma_allocate(vma, VmOptions::Specific, SPOT, PAGE, &guard, &at), Status::Ok);
    ours::VirtAddr mapped = 0;
    ok &= expect("shell: vma_map(Specific, over the guard) = ",
                 ou::vma_map(vma, SPECIFIC_R, SPOT, vmo, 0, PAGE, &mapped), Status::AlreadyExists);
    ok &= expect("shell: vma_map(into the guard) = ",
                 ou::vma_map(guard, VmOptions::PermRead, 0, vmo, 0, PAGE, &mapped),
                 Status::AccessDenied);
    ok &= expect("shell: close(guard) = ", ou::handle_close(guard), Status::Ok);
    ok &= expect("shell: vma_map(Specific, over the guard, handle closed) = ",
                 ou::vma_map(vma, SPECIFIC_R, SPOT, vmo, 0, PAGE, &mapped), Status::AlreadyExists);

    // The options and the arguments.
    constexpr auto ELSEWHERE = SPOT + 16 * PAGE;
    ok &= expect("shell: vma_allocate(Specific | CanMapRead) = ",
                 ou::vma_allocate(vma, VmOptions::Specific | VmOptions::CanMapRead, ELSEWHERE, PAGE,
                                  &guard, &at),
                 Status::NotSupported);
    ok &=
        expect("shell: vma_allocate(without Specific) = ",
               ou::vma_allocate(vma, VmOptions::None, 0, PAGE, &guard, &at), Status::NotSupported);
    ok &= expect("shell: vma_allocate(Specific | PermRead) = ",
                 ou::vma_allocate(vma, SPECIFIC_R, ELSEWHERE, PAGE, &guard, &at), Status::InvArgs);
    ok &= expect("shell: vma_allocate(size 0) = ",
                 ou::vma_allocate(vma, VmOptions::Specific, ELSEWHERE, 0, &guard, &at),
                 Status::InvArgs);
    ok &= expect("shell: vma_allocate(unaligned offset) = ",
                 ou::vma_allocate(vma, VmOptions::Specific, ELSEWHERE + 8, PAGE, &guard, &at),
                 Status::InvArgs);

    // A child_addr the kernel cannot write: the range goes back, so the same
    // spot can be taken right after.
    ok &= expect("shell: vma_allocate(child_addr = kernel half) = ",
                 ou::vma_allocate(vma, VmOptions::Specific, ELSEWHERE, PAGE, &guard,
                                  reinterpret_cast<ours::VirtAddr *>(KERNEL)),
                 Status::InvArgs);
    ok &= expect("shell: vma_allocate(the same spot) = ",
                 ou::vma_allocate(vma, VmOptions::Specific, ELSEWHERE, PAGE, &guard, &at),
                 Status::Ok);
    ok &= expect("shell: close(guard) = ", ou::handle_close(guard), Status::Ok);
    ok &= expect("shell: close(vmo) = ", ou::handle_close(vmo), Status::Ok);

    if (ok) {
        static_cast<void>(ou::debug_line("shell: vma_allocate checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: vma_allocate checks FAILED"));
    }
}

/// process_create, and what closing the last handle to a process that was
/// never started does (K23a).
///
/// Runs where userboot started this program: the root job is what a process
/// is created under. What it pins down:
///   - the job: a value that names nothing is BadHandle, a channel BadType,
///     a copy without ManageProcess AccessDenied; an option bit is InvArgs;
///   - the handles: two distinct values come out, one to the process and one
///     to its root VMA, and closing both is Ok;
///   - the safety net: with a second handle to the process, closing the first
///     leaves it Initial -- it does not raise TERMINATED -- and only closing
///     the last takes it Dead. The kernel log shows the address space and the
///     process destroyed then (`mm: aspace ... destroyed`, `object: process
///     ... destroyed`), which this program cannot see;
///   - a value the kernel cannot write back leaves nothing behind: failing
///     more times than the quota allows and then succeeding once shows no
///     handle was left in the table by any of them, and the log shows every
///     process made by a failed call destroyed.
auto check_process_create(ours::HandleValue const job) -> void {
    using ours::Rights;
    using ours::Signals;
    using ours::Status;

    constexpr ou::Str NAME{"created"};

    if (job == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: process_create checks skipped (no root job)"));
        return;
    }

    auto ok = true;
    ours::HandleValue process = ours::HANDLE_INVALID;
    ours::HandleValue region = ours::HANDLE_INVALID;

    ok &= expect("shell: process_create(HANDLE_INVALID) = ",
                 ou::process_create(ours::HANDLE_INVALID, NAME, &process, &region),
                 Status::BadHandle);
    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        ok &= expect("shell: process_create(job = a channel) = ",
                     ou::process_create(a, NAME, &process, &region), Status::BadType);
        ok &= expect("shell: close(a) = ", ou::handle_close(a), Status::Ok);
        ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);
    } else {
        ok = false;
    }
    ours::HandleValue weak = ours::HANDLE_INVALID;
    ok &= expect("shell: handle_duplicate(job, without ManageProcess) = ",
                 ou::handle_duplicate(job, Rights::Basic, &weak), Status::Ok);
    ok &= expect("shell: process_create(job without ManageProcess) = ",
                 ou::process_create(weak, NAME, &process, &region), Status::AccessDenied);
    ok &= expect("shell: close(the weak job) = ", ou::handle_close(weak), Status::Ok);
    ok &= expect("shell: process_create(an option bit) = ",
                 ours_process_create(job, NAME.data(), NAME.size(),
                                     static_cast<ours::ProcessCreateOptions>(1), &process, &region),
                 Status::InvArgs);

    // Two handles, and the safety net.
    if (expect("shell: process_create = ", ou::process_create(job, NAME, &process, &region),
               Status::Ok)) {
        ok &=
            process != ours::HANDLE_INVALID && region != ours::HANDLE_INVALID && process != region;
        ours::HandleValue second = ours::HANDLE_INVALID;
        ok &= expect("shell: handle_duplicate(process) = ",
                     ou::handle_duplicate(process, Rights::SameRights, &second), Status::Ok);
        ok &= expect("shell: close(region) = ", ou::handle_close(region), Status::Ok);
        ok &= expect("shell: close(the first process handle) = ", ou::handle_close(process),
                     Status::Ok);
        // One handle left: still Initial, so not terminated.
        auto observed = Signals::None;
        ok &=
            expect("shell: wait_one(process, TERMINATED, poll) with a handle left = ",
                   ou::object_wait_one(second, Signals::Terminated, 0, &observed), Status::TimeOut);
        ok &= expect("shell: close(the last process handle) = ", ou::handle_close(second),
                     Status::Ok);
    } else {
        ok = false;
    }

    // Nothing left behind by a call that fails after the process is made:
    // the table is filled to the quota except for exactly two slots, which is
    // what one call needs, and then calls that fail are made more than once.
    // A call that left its two handles behind would leave the next one none,
    // and it would answer NoResource instead of InvArgs. (Failing quota-plus-
    // one times instead would show the same thing but makes a process for
    // each, which is slow enough to time the debug image out.)
    constexpr u32 ROOM = ours::HANDLE_MAX_PER_PROCESS + 2;
    static ours::HandleValue filler[ROOM];
    u32 held = 0;
    while (held + 2 <= ROOM && ou::channel_create(&filler[held], &filler[held + 1]) == Status::Ok) {
        held += 2;
    }
    // A pair needs two slots, so at most one is left; fill it too.
    if (held < ROOM && ou::handle_duplicate(job, Rights::SameRights, &filler[held]) == Status::Ok) {
        held += 1;
    }
    ok &= held >= 2;
    if (held >= 2) {
        ok &= ou::handle_close(filler[held - 1]) == Status::Ok;
        ok &= ou::handle_close(filler[held - 2]) == Status::Ok;
        held -= 2;
        for (u32 i = 0; i < 3; ++i) {
            ok &= expect(
                "shell: process_create(process_out = kernel half), table nearly full = ",
                ours_process_create(job, NAME.data(), NAME.size(), ours::ProcessCreateOptions::None,
                                    reinterpret_cast<ours::HandleValue *>(KERNEL), &region),
                Status::InvArgs);
        }
        ok &= expect("shell: process_create after that = ",
                     ou::process_create(job, NAME, &process, &region), Status::Ok);
        ok &= expect("shell: close(region) = ", ou::handle_close(region), Status::Ok);
        ok &= expect("shell: close(process) = ", ou::handle_close(process), Status::Ok);
    }
    for (u32 i = 0; i < held; ++i) {
        ok &= ou::handle_close(filler[i]) == Status::Ok;
    }

    if (ok) {
        static_cast<void>(ou::debug_line("shell: process_create checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: process_create checks FAILED"));
    }
}

/// thread_create (K23a): a thread that has not started is a dispatcher and
/// nothing else.
///
/// Runs where userboot started this program, on a process of its own made
/// here and never started. What it pins down:
///   - the handles: none, a VMA handle in the process's place, and a process
///     handle without ManageThread are BadHandle, BadType and AccessDenied;
///     an option bit is InvArgs;
///   - the out-pointer: one the kernel cannot write is InvArgs and leaves no
///     handle behind (three failures with the table full to two slots, as for
///     process_create);
///   - the lifetime: a thread's handle closed unstarted is Ok, and so is
///     closing the process's while a thread handle to it is still held --
///     the process, never started, dies with its last *handle*, whatever
///     still points at the object. The kernel log shows every thread here
///     destroyed and none given a kernel thread: it is made by start, which
///     nothing here calls.
auto check_thread_create(ours::HandleValue const job) -> void {
    using ours::Rights;
    using ours::Status;

    constexpr ou::Str NAME{"never-started"};

    if (job == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: thread_create checks skipped (no root job)"));
        return;
    }

    auto ok = true;
    ours::HandleValue process = ours::HANDLE_INVALID;
    ours::HandleValue region = ours::HANDLE_INVALID;
    if (!expect("shell: process_create = ", ou::process_create(job, "host", &process, &region),
                Status::Ok)) {
        static_cast<void>(ou::debug_line("shell: thread_create checks FAILED"));
        return;
    }

    ours::HandleValue thread = ours::HANDLE_INVALID;
    ok &= expect("shell: thread_create(HANDLE_INVALID) = ",
                 ou::thread_create(ours::HANDLE_INVALID, NAME, &thread), Status::BadHandle);
    ok &= expect("shell: thread_create(process = its root VMA) = ",
                 ou::thread_create(region, NAME, &thread), Status::BadType);
    ours::HandleValue weak = ours::HANDLE_INVALID;
    ok &= expect("shell: handle_duplicate(process, without ManageThread) = ",
                 ou::handle_duplicate(process, Rights::Basic, &weak), Status::Ok);
    ok &= expect("shell: thread_create(process without ManageThread) = ",
                 ou::thread_create(weak, NAME, &thread), Status::AccessDenied);
    ok &= expect("shell: close(the weak process handle) = ", ou::handle_close(weak), Status::Ok);
    ok &= expect("shell: thread_create(an option bit) = ",
                 ours_thread_create(process, NAME.data(), NAME.size(),
                                    static_cast<ours::ThreadCreateOptions>(1), &thread),
                 Status::InvArgs);
    ok &= expect("shell: close(region) = ", ou::handle_close(region), Status::Ok);

    // A thread made and closed, more than once.
    for (u32 i = 0; i < 4; ++i) {
        if (expect("shell: thread_create = ", ou::thread_create(process, NAME, &thread),
                   Status::Ok)) {
            ok &= expect("shell: close(the unstarted thread) = ", ou::handle_close(thread),
                         Status::Ok);
        } else {
            ok = false;
        }
    }

    // The table full to exactly two slots, then failures that must leave none
    // behind: a thread needs one, so this is stricter than it needs to be, and
    // it is the same shape as process_create's, so the two read alike.
    constexpr u32 ROOM = ours::HANDLE_MAX_PER_PROCESS + 2;
    static ours::HandleValue filler[ROOM];
    u32 held = 0;
    while (held + 2 <= ROOM && ou::channel_create(&filler[held], &filler[held + 1]) == Status::Ok) {
        held += 2;
    }
    if (held < ROOM && ou::handle_duplicate(job, Rights::SameRights, &filler[held]) == Status::Ok) {
        held += 1;
    }
    ok &= held >= 2;
    if (held >= 2) {
        ok &= ou::handle_close(filler[held - 1]) == Status::Ok;
        held -= 1;
        for (u32 i = 0; i < 3; ++i) {
            ok &= expect("shell: thread_create(out = kernel half), table nearly full = ",
                         ours_thread_create(process, NAME.data(), NAME.size(),
                                            ours::ThreadCreateOptions::None,
                                            reinterpret_cast<ours::HandleValue *>(KERNEL)),
                         Status::InvArgs);
        }
        ok &=
            expect("shell: thread_create after that = ", ou::thread_create(process, NAME, &thread),
                   Status::Ok);
    }
    for (u32 i = 0; i < held; ++i) {
        ok &= ou::handle_close(filler[i]) == Status::Ok;
    }

    // The process's last handle first, with a thread handle still open.
    ok &= expect("shell: close(the process) = ", ou::handle_close(process), Status::Ok);
    ok &= expect("shell: close(the thread, after its process) = ", ou::handle_close(thread),
                 Status::Ok);

    if (ok) {
        static_cast<void>(ou::debug_line("shell: thread_create checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: thread_create checks FAILED"));
    }
}

/// process_start (K23a): the handle argument moves, and a start that fails
/// leaves the process as it was.
///
/// Runs where userboot started this program, on processes made here. None of
/// them can be started: no vDSO is mapped into them yet (that is what makes a
/// process able to make a system call, and vma_map does not register one
/// yet), so every well-formed call answers BadState. That is what lets this
/// check the failure paths, which are what the call is careful about:
///   - the handles named: none is BadHandle, a VMA in the process's or a
///     process in the thread's place BadType, a process without Write
///     AccessDenied, another process's thread AccessDenied;
///   - the handle moved: one that names nothing is BadHandle, one without
///     Transfer AccessDenied and gone all the same;
///   - the handle consumed on failure: a channel end passed in is closed by
///     the failed start -- its peer reads PEER_CLOSED, which is the only way
///     from here to see that nothing was left behind in the process's table --
///     and the same on a second try, so the process is as it was;
///   - the resolving order: the thread's own handle passed as the handle
///     argument is consumed, and does not stop the call being answered as
///     any other would be.
auto check_process_start(ours::HandleValue const job) -> void {
    using ours::Rights;
    using ours::Signals;
    using ours::Status;

    constexpr auto ENTRY = ours::VirtAddr{0x400000};
    constexpr auto STACK = ours::VirtAddr{0x500000};

    if (job == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: process_start checks skipped (no root job)"));
        return;
    }

    auto ok = true;
    ours::HandleValue process = ours::HANDLE_INVALID;
    ours::HandleValue region = ours::HANDLE_INVALID;
    ours::HandleValue other = ours::HANDLE_INVALID;
    ours::HandleValue other_region = ours::HANDLE_INVALID;
    ours::HandleValue thread = ours::HANDLE_INVALID;
    ours::HandleValue other_thread = ours::HANDLE_INVALID;
    if (!expect("shell: process_create = ", ou::process_create(job, "starts", &process, &region),
                Status::Ok) ||
        !expect("shell: process_create = ", ou::process_create(job, "other", &other, &other_region),
                Status::Ok) ||
        !expect("shell: thread_create = ", ou::thread_create(process, "first", &thread),
                Status::Ok) ||
        !expect("shell: thread_create = ", ou::thread_create(other, "theirs", &other_thread),
                Status::Ok)) {
        static_cast<void>(ou::debug_line("shell: process_start checks FAILED"));
        return;
    }

    // The handles named.
    auto const H = ours::HANDLE_INVALID;
    ok &= expect("shell: process_start(HANDLE_INVALID) = ",
                 ou::process_start(H, thread, ENTRY, STACK, H, 0), Status::BadHandle);
    ok &= expect("shell: process_start(process = its root VMA) = ",
                 ou::process_start(region, thread, ENTRY, STACK, H, 0), Status::BadType);
    ok &= expect("shell: process_start(thread = a process) = ",
                 ou::process_start(process, process, ENTRY, STACK, H, 0), Status::BadType);
    ours::HandleValue weak = ours::HANDLE_INVALID;
    ok &= expect("shell: handle_duplicate(process, without Write) = ",
                 ou::handle_duplicate(process, Rights::Basic, &weak), Status::Ok);
    ok &= expect("shell: process_start(process without Write) = ",
                 ou::process_start(weak, thread, ENTRY, STACK, H, 0), Status::AccessDenied);
    ok &= expect("shell: close(the weak process handle) = ", ou::handle_close(weak), Status::Ok);
    ok &=
        expect("shell: process_start(another process's thread) = ",
               ou::process_start(process, other_thread, ENTRY, STACK, H, 0), Status::AccessDenied);

    // The handle moved.
    ours::HandleValue a = ours::HANDLE_INVALID;
    ours::HandleValue b = ours::HANDLE_INVALID;
    if (!expect("shell: channel_create = ", ou::channel_create(&a, &b), Status::Ok)) {
        ok = false;
    } else {
        ours::HandleValue stale = b;
        ok &= expect("shell: close(b) = ", ou::handle_close(b), Status::Ok);
        ok &= expect("shell: process_start(a handle that names nothing) = ",
                     ou::process_start(process, thread, ENTRY, STACK, stale, 0), Status::BadHandle);

        // A VMO's handle can be duplicated (a channel's cannot), so it is what
        // gives a copy without Transfer.
        ours::HandleValue vmo = ours::HANDLE_INVALID;
        ours::HandleValue untransferable = ours::HANDLE_INVALID;
        if (expect("shell: vmo_create = ", ou::vmo_create(4096, ours::VmoOptions::None, &vmo),
                   Status::Ok) &&
            expect("shell: handle_duplicate(vmo, without Transfer) = ",
                   ou::handle_duplicate(vmo, Rights::Duplicate | Rights::Read, &untransferable),
                   Status::Ok)) {
            ok &= expect("shell: process_start(a handle without Transfer) = ",
                         ou::process_start(process, thread, ENTRY, STACK, untransferable, 0),
                         Status::AccessDenied);
            ok &= expect("shell: close(that handle, gone all the same) = ",
                         ou::handle_close(untransferable), Status::BadHandle);
        } else {
            ok = false;
        }
        ok &= expect("shell: close(vmo) = ", ou::handle_close(vmo), Status::Ok);
        ok &= expect("shell: close(a) = ", ou::handle_close(a), Status::Ok);
    }

    // Consumed and closed on failure, twice: the process is as it was.
    for (u32 i = 0; i < 2; ++i) {
        ours::HandleValue c = ours::HANDLE_INVALID;
        ours::HandleValue d = ours::HANDLE_INVALID;
        if (!expect("shell: channel_create = ", ou::channel_create(&c, &d), Status::Ok)) {
            ok = false;
            break;
        }
        ok &= expect("shell: process_start(no vDSO in the process) = ",
                     ou::process_start(process, thread, ENTRY, STACK, c, 0), Status::BadState);
        auto observed = Signals::None;
        ok &= expect("shell: wait_one(the peer, PEER_CLOSED, poll) = ",
                     ou::object_wait_one(d, Signals::PeerClosed, 0, &observed), Status::Ok);
        ok &= !!(observed & Signals::PeerClosed);
        ok &= expect("shell: close(c, gone) = ", ou::handle_close(c), Status::BadHandle);
        ok &= expect("shell: close(d) = ", ou::handle_close(d), Status::Ok);
    }

    // The thread's own handle as the handle argument.
    ok &= expect("shell: process_start(arg1 = the thread's own handle) = ",
                 ou::process_start(process, thread, ENTRY, STACK, thread, 0), Status::BadState);
    ok &= expect("shell: close(the thread, gone) = ", ou::handle_close(thread), Status::BadHandle);

    ok &= expect("shell: close(other thread) = ", ou::handle_close(other_thread), Status::Ok);
    ok &= expect("shell: close(region) = ", ou::handle_close(region), Status::Ok);
    ok &= expect("shell: close(other region) = ", ou::handle_close(other_region), Status::Ok);
    ok &= expect("shell: close(process) = ", ou::handle_close(process), Status::Ok);
    ok &= expect("shell: close(other process) = ", ou::handle_close(other), Status::Ok);

    if (ok) {
        static_cast<void>(ou::debug_line("shell: process_start checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: process_start checks FAILED"));
    }
}

/// Mapping the vDSO's VMO: the mapping is the registration of a process's vDSO
/// range, so it is allowed in one shape only (K23a).
///
/// Runs where userboot started this program, which is handed the VMO. On
/// processes made here and never started, so what is checked is what the
/// kernel says about the mapping, not what a running process does with it:
///   - the rights: the handle has no Write, so a writable mapping is
///     AccessDenied before the vDSO is looked at;
///   - the shape, all InvArgs: not sealed (the launcher asks for it, and the
///     kernel refuses the vDSO without it, so a launcher that forgot fails
///     here and not silently); read only, not execute; a part of the VMO
///     rather than the whole (an offset past its start, and every length
///     short of its size -- the loop that finds the size is this check); and
///     the whole VMO mapped once in the allowed shape is Ok;
///   - once per address space: a second mapping there is AccessDenied, while
///     another process's first is Ok -- the claim is the address space's;
///   - by identity, not by content: a slice of the VMO, which has the same
///     pages, maps as an ordinary mapping and registers nothing, so a process
///     that has only that is still refused by process_start (BadState, no
///     vDSO), where one that mapped the VMO itself would not be.
auto check_vdso_map(ours::HandleValue const job, ours::HandleValue const vdso) -> void {
    using ours::Status;
    using ours::VmOptions;

    constexpr auto PAGE = u64{4096};
    constexpr auto RX = VmOptions::PermRead | VmOptions::PermExecute;
    constexpr auto SEALED_RX = RX | VmOptions::Sealed;
    constexpr auto SEALED_RW = VmOptions::PermRead | VmOptions::PermWrite | VmOptions::Sealed;

    if (job == ours::HANDLE_INVALID || vdso == ours::HANDLE_INVALID) {
        static_cast<void>(ou::debug_line("shell: vdso map checks skipped (no root job or vDSO)"));
        return;
    }

    auto ok = true;
    ours::HandleValue p = ours::HANDLE_INVALID;
    ours::HandleValue p_vma = ours::HANDLE_INVALID;
    ours::HandleValue q = ours::HANDLE_INVALID;
    ours::HandleValue q_vma = ours::HANDLE_INVALID;
    ours::HandleValue s = ours::HANDLE_INVALID;
    ours::HandleValue s_vma = ours::HANDLE_INVALID;
    if (!expect("shell: process_create = ", ou::process_create(job, "vdso-p", &p, &p_vma),
                Status::Ok) ||
        !expect("shell: process_create = ", ou::process_create(job, "vdso-q", &q, &q_vma),
                Status::Ok) ||
        !expect("shell: process_create = ", ou::process_create(job, "vdso-s", &s, &s_vma),
                Status::Ok)) {
        static_cast<void>(ou::debug_line("shell: vdso map checks FAILED"));
        return;
    }

    ours::VirtAddr at = 0;
    ok &= expect("shell: vma_map(vdso, RW, sealed) = ",
                 ou::vma_map(p_vma, SEALED_RW, 0, vdso, 0, PAGE, &at), Status::AccessDenied);
    ok &= expect("shell: vma_map(vdso, RX, not sealed) = ",
                 ou::vma_map(p_vma, RX, 0, vdso, 0, PAGE, &at), Status::InvArgs);
    ok &= expect("shell: vma_map(vdso, read only, sealed) = ",
                 ou::vma_map(p_vma, VmOptions::PermRead | VmOptions::Sealed, 0, vdso, 0, PAGE, &at),
                 Status::InvArgs);
    ok &= expect("shell: vma_map(vdso, from an offset) = ",
                 ou::vma_map(p_vma, SEALED_RX, 0, vdso, PAGE, PAGE, &at), Status::InvArgs);

    // Every length short of the whole is refused; the first that is not is
    // the whole, and it is mapped.
    u64 whole = 0;
    for (u64 pages = 1; pages <= 64 && whole == 0; ++pages) {
        auto const status = ou::vma_map(p_vma, SEALED_RX, 0, vdso, 0, pages * PAGE, &at);
        if (status == Status::Ok) {
            whole = pages * PAGE;
        } else if (status != Status::InvArgs) {
            ok = expect("shell: vma_map(vdso, a part of it) = ", status, Status::InvArgs);
            break;
        }
    }
    ok &= whole != 0;
    char pages_text[10];
    static_cast<void>(
        ou::debug_line({"shell: the vDSO is ",
                        ou::decimal(static_cast<u32>(whole / PAGE), pages_text), " page(s)"}));

    if (whole != 0) {
        ours::VirtAddr again = 0;
        ok &=
            expect("shell: vma_map(vdso) a second time = ",
                   ou::vma_map(p_vma, SEALED_RX, 0, vdso, 0, whole, &again), Status::AccessDenied);
        ok &= expect("shell: vma_map(vdso) into another process = ",
                     ou::vma_map(q_vma, SEALED_RX, 0, vdso, 0, whole, &again), Status::Ok);

        // The same pages by way of a slice: a mapping, and no registration.
        ours::HandleValue slice = ours::HANDLE_INVALID;
        ok &= expect("shell: vmo_create_child(vdso, Slice) = ",
                     ou::vmo_create_child(vdso, ours::VmoChildOptions::Slice, 0, whole, &slice),
                     Status::Ok);
        ok &= expect("shell: vma_map(the slice, RX) = ",
                     ou::vma_map(s_vma, RX, 0, slice, 0, whole, &again), Status::Ok);
        ok &= expect("shell: close(the slice) = ", ou::handle_close(slice), Status::Ok);

        // Sealing is not the vDSO's alone: any mapping may ask for it, and an
        // ordinary VMO's is accepted. What a sealed mapping then refuses is
        // checked in the kernel (vmaseal.test), since no program can unmap or
        // protect yet.
        ours::HandleValue plain = ours::HANDLE_INVALID;
        ok &= expect("shell: vmo_create = ", ou::vmo_create(PAGE, ours::VmoOptions::None, &plain),
                     Status::Ok);
        ok &= expect(
            "shell: vma_map(an ordinary VMO, sealed) = ",
            ou::vma_map(q_vma, VmOptions::PermRead | VmOptions::Sealed, 0, plain, 0, PAGE, &again),
            Status::Ok);
        ok &= expect("shell: close(the VMO) = ", ou::handle_close(plain), Status::Ok);

        ours::HandleValue t = ours::HANDLE_INVALID;
        ok &= expect("shell: thread_create = ", ou::thread_create(s, "first", &t), Status::Ok);
        ok &= expect("shell: process_start(a slice of the vDSO mapped, no vDSO) = ",
                     ou::process_start(s, t, 0x400000, 0x500000, ours::HANDLE_INVALID, 0),
                     Status::BadState);
        ok &= expect("shell: close(the thread) = ", ou::handle_close(t), Status::Ok);
    }

    for (auto const handle: {p_vma, q_vma, s_vma, p, q, s}) {
        ok &= ou::handle_close(handle) == Status::Ok;
    }

    if (ok) {
        static_cast<void>(ou::debug_line("shell: vdso map checks passed"));
    } else {
        static_cast<void>(ou::debug_line("shell: vdso map checks FAILED"));
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
    // The vDSO's VMO, from userboot: what a launcher maps into each process it
    // starts. Only the shell userboot starts holds it.
    auto const vdso_vmo = runtime::take_startup_handle(ours::HandleType::VmoVdso, 0);
    // The root VMA, which whoever started this put in the processargs.
    auto const vma = runtime::take_startup_handle(ours::HandleType::VmaRoot, 0);
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
    // Before the job is closed below: it is what a process is created under.
    check_process_create(job);
    check_thread_create(job);
    check_process_start(job);
    check_vdso_map(job, vdso_vmo);
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
    check_vmo_create_child(bootfs);
    check_vmo_replace_as_executable(bootfs, vmex);
    check_vma_map(vma, bootfs, vmex);
    check_vma_allocate(vma);
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
