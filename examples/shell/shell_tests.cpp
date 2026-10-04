/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "shell_tests.hpp"

#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/syscall/resource.hpp>
#include <ours/vdso.hpp>

#include <algorithm>
#include <iterator>
#include <span>
#include <string_view>

#include <cstring>

#include "programs.hpp"
#include <bootfs/format.hpp>
#include <launch/program.hpp>
#include <ort/debug.hpp>
#include <ort/startup.hpp>

namespace {
using shell::spells;

auto report(std::string_view const what, ours::Status const status) -> void {
    auto _ = ort::debug_line({what, std::string_view{ours::to_string(status)}});
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
auto check_handle_close(ours::Uandle const startup) -> void {
    report("shell: close(startup handle) = ", ours::handle_close(startup));
    report("shell: close again = ", ours::handle_close(startup));
}

/// Prints "<what><got>" and whether that was |want|; answers whether it was.
auto expect(std::string_view const what, ours::Status const got, ours::Status const want) -> bool {
    auto const got_name = std::string_view{ours::to_string(got)};
    if (got == want) {
        auto _ = ort::debug_line({what, got_name});
        return true;
    }
    auto const want_name = std::string_view{ours::to_string(want)};
    auto _ = ort::debug_line({what, got_name, " (expected ", want_name, ")"});
    return false;
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
    /// The process, as the launcher handed it back; Uandle::Invalid when it
    /// did not start. The caller owns it.
    ours::Uandle process;
    /// The exception channel bound to it behaved: bound, refused a second,
    /// refused a write, delivered whole messages, and closed once the probe
    /// was dead.
    bool exception_channel_ok;
    /// How many exceptions the channel delivered, and the type of the last.
    u32 exceptions_seen;
    ours::ExceptionType exception_type;
};

/// Binds an exception channel to |process| and checks what that promises
/// before anything is sent on it: a second channel is refused while this one
/// is bound, and the end handed back reads but does not write. The channel
/// goes to |out|; false, with the reason reported, when anything differed.
/// Step 4 of .agents/tasks/exception-channels/PLAN.md.
auto bind_exception_channel(ours::Uandle const process, ours::Uandle *const out) -> bool {
    using ours::Status;

    auto const bound =
        ours::task_create_exception_channel(process, ours::ExceptionChannelOptions::None, out);
    if (bound != Status::Ok) {
        report("shell: task_create_exception_channel = ", bound);
        return false;
    }

    auto ok = true;
    auto second = ours::Uandle::Invalid;
    if (auto const again = ours::task_create_exception_channel(
            process, ours::ExceptionChannelOptions::None, &second);
        again != Status::AlreadyBound) {
        report("shell: a second exception channel = ", again);
        auto _ = ours::handle_close(second);
        ok = false;
    }

    // The kernel's end never reads: anything written to it would stay there.
    constexpr char BYTE = 'x';
    if (auto const wrote =
            ours::channel_write(*out, ours::ChannelWriteOptions::None, &BYTE, 1, nullptr, 0);
        wrote != Status::AccessDenied) {
        report("shell: channel_write on an exception channel = ", wrote);
        ok = false;
    }
    return ok;
}

/// Answers every exception |channel| delivers by passing it on, until the
/// process it is bound to is dead and the channel closes, and counts them
/// into |run|. A faulting probe stops in the kernel until its exception is
/// answered, so this has to come before waiting for the probe's own answer.
/// False, with the reason reported, when the channel misbehaved. Step 5 of
/// .agents/tasks/exception-channels/PLAN.md.
auto pass_on_exceptions(ours::Uandle const channel, ProbeRun *const run) -> bool {
    using ours::Signals;
    using ours::Status;

    for (;;) {
        auto seen = Signals::None;
        auto const waited = ours::object_wait_one(channel, Signals::Readable | Signals::PeerClosed,
                                                  ours::INSTANT_INFINITE, &seen);
        if (waited != Status::Ok) {
            report("shell: waiting on the exception channel = ", waited);
            return false;
        }
        if (!(seen & Signals::Readable)) {
            // Closed, with nothing left in it: the process is dead.
            return true;
        }

        ours::ExceptionInfo info{};
        auto exception = ours::Uandle::Invalid;
        u32 bytes = 0;
        u32 handles = 0;
        auto const read = ours::channel_read(channel, ours::ChannelReadOptions::None, &info,
                                             &exception, sizeof(info), 1, &bytes, &handles);
        if (read != Status::Ok || bytes != sizeof(info) || handles != 1) {
            report("shell: reading an exception = ", read);
            auto _ = ours::handle_close(exception);
            return false;
        }
        ++run->exceptions_seen;
        run->exception_type = info.type;

        // Closed with no state set: TryNext. No other handler is bound, so
        // the kernel kills the probe, and the channel closes with it.
        auto _ = ours::handle_close(exception);
    }
}

/// Starts fault_probe under |job| with |probe| as argv[1] with |launcher|,
/// and waits for it to answer or die. The handle to the process the launcher
/// returns is handed back in run.process.
///
/// The probe is started the way every program on the runtime is: a
/// processargs message on its bootstrap channel (argv = {"bin/fault_probe",
/// probe}), written once the process exists -- its root VMA, which the
/// launcher hands back, is one of the two startup handles -- with the far end of a
/// second channel, as User0, for the answer. The answer
/// cannot come back on the bootstrap channel, because the runtime closes that
/// as soon as it has read the message: waiting on it, this would see
/// PeerClosed before the probe had even reached main and count every probe
/// as killed.
auto run_probe(ours::Uandle const job, std::string_view const probe,
               shell::LaunchProgram const launcher, void *const launcher_context) -> ProbeRun {
    using ours::Signals;
    using ours::Status;

    ProbeRun run{};

    // The message first, while this holds no handles: a name that does not
    // encode leaves nothing to clean up.
    u8 message[128];
    u32 length = 0;
    u32 const infos[] = {ours::handle_info(ours::HandleType::User0, 0),
                         ours::handle_info(ours::HandleType::VmaRoot, 0)};
    std::string_view const args[] = {user_programs::FAULT_PROBE, {probe.data(), probe.size()}};
    if (auto const status = ours::encode_process_args(message, infos, args, {}, {}, &length);
        status != Status::Ok) {
        report("shell: encoding the probe's processargs = ", status);
        return run;
    }

    ours::Uandle answer = ours::Uandle::Invalid;
    ours::Uandle answer_there = ours::Uandle::Invalid;
    ours::Uandle bootstrap = ours::Uandle::Invalid;
    ours::Uandle bootstrap_there = ours::Uandle::Invalid;
    if (!expect("shell: probe channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &answer, &answer_there),
                Status::Ok)) {
        return run;
    }
    if (!expect(
            "shell: probe channel_create = ",
            ours::channel_create(ours::ChannelCreateOptions::None, &bootstrap, &bootstrap_there),
            Status::Ok)) {
        auto _ = ours::handle_close(answer);
        auto _ = ours::handle_close(answer_there);
        return run;
    }

    // The process first: |bootstrap_there| leaves this process whatever
    // happens, and its root VMA comes back for the message.
    ours::Uandle root_vma = ours::Uandle::Invalid;
    ours::Uandle process = ours::Uandle::Invalid;
    auto status = Status::Ok;
    auto const started =
        launcher(launcher_context, job, user_programs::FAULT_PROBE, bootstrap_there);
    if (started.is_err()) {
        status = started.unwrap_err();
    } else {
        process = started.unwrap().process;
        root_vma = started.unwrap().root_vma;
    }
    if (status != Status::Ok) {
        report("shell: starting fault_probe = ", status);
        auto _ = ours::handle_close(bootstrap);
        auto _ = ours::handle_close(answer);
        auto _ = ours::handle_close(answer_there);
        return run;
    }
    run.started = true;

    // An exception channel on the probe before it can fault: it waits in its
    // runtime for the processargs written below.
    auto exceptions = ours::Uandle::Invalid;
    run.exception_channel_ok = bind_exception_channel(process, &exceptions);

    // Then the message. Both handles leave this process with it, whatever the
    // write answers; the probe waits for it, and if it never comes, sees this
    // end close and ends itself.
    ours::Uandle handed[] = {answer_there, root_vma};
    status =
        ours::channel_write(bootstrap, ours::ChannelWriteOptions::None, message, length, handed, 2);
    // This end has nothing more to say, and holding it would keep a handle
    // charged to this process for nothing.
    auto _ = ours::handle_close(bootstrap);
    if (status != Status::Ok) {
        report("shell: writing the probe's processargs = ", status);
    }

    // The exceptions first: a probe that faults waits in the kernel for its
    // answer, and would never get to the answer channel below.
    if (exceptions != ours::Uandle::Invalid) {
        run.exception_channel_ok &= pass_on_exceptions(exceptions, &run);
        auto _ = ours::handle_close(exceptions);
    }

    auto observed = Signals::None;
    auto _ = ours::object_wait_one(answer, Signals::Readable | Signals::PeerClosed,
                                   ours::INSTANT_INFINITE, &observed);
    run.killed = !(observed & Signals::Readable);
    if (!run.killed) {
        // An answer that does not fit, or a read that fails, leaves no answer:
        // the size the kernel writes back on BufferTooSmall is what it would
        // need, not what is in run.answer.
        if (auto const read =
                ours::channel_read(answer, ours::ChannelReadOptions::None, run.answer, nullptr,
                                   sizeof(run.answer), 0, &run.answer_size, nullptr);
            read != Status::Ok) {
            report("shell: reading the probe's answer = ", read);
            run.answer_size = 0;
        }

        // And then until the probe has let go of its end. That end was made
        // here, so it is charged to this process (the per-creator handle
        // quota) for as long as it lives, and a probe that answered is still
        // on its way out: return now and the quota check further on counts
        // it -- one handle short, depending on how fast the probe dies. A
        // killed probe's end has closed already; that is what woke the wait.
        //
        // This narrows the window rather than closing it: the kernel raises
        // PEER_CLOSED in its object::Handle destructor before it refunds the
        // handle to its creator (docs/repairs/260926/object.md), so for a few
        // instructions after this wakes the handle may still be counted. The
        // checks that run before the quota check are what make that gap
        // unobservable today.
        auto _ =
            ours::object_wait_one(answer, Signals::PeerClosed, ours::INSTANT_INFINITE, &observed);
    }
    auto _ = ours::handle_close(answer);

    run.process = process;
    return run;
}

/// object_get_info on a process that has ended (K24): the control probe, the
/// one probe that ends by returning from main, so the one whose code is its
/// own rather than the kernel's.
///
/// TERMINATED first. The probe answers before it has finished dying, and
/// until it has, the return code is not final; TERMINATED is raised last,
/// once nothing can change it (ProcessDispatcher::finish_dead_transition()).
///
/// Then the record, and every way the call refuses: a buffer one byte short,
/// a topic this kernel does not answer, an object that is not a process (the
/// job), and a handle without Inspect.
auto check_object_get_info(ours::Uandle const process, ours::Uandle const job) -> void {
    using ours::InfoProcess;
    using ours::InfoProcessFlags;
    using ours::InfoTopic;
    using ours::Signals;
    using ours::Status;

    auto ok =
        expect("shell: wait_one(the control, Terminated) = ",
               ours::object_wait_one(process, Signals::Terminated, ours::INSTANT_INFINITE, nullptr),
               Status::Ok);

    InfoProcess info{};
    usize actual = 0;
    usize avail = 0;
    ok &= expect(
        "shell: object_get_info(the control, Process) = ",
        ours::object_get_info(process, InfoTopic::Process, &info, sizeof(info), &actual, &avail),
        Status::Ok);
    // fault_probe's main returns UNKNOWN, 2, for a probe name it does not
    // know, which is what makes the control the one run that ends by
    // returning from main.
    constexpr auto UNKNOWN = i64{2};
    constexpr auto ENDED = InfoProcessFlags::Started | InfoProcessFlags::Exited;
    if (info.return_code == UNKNOWN && info.flags == ENDED && actual == 1 && avail == 1) {
        auto _ = ort::debug_line("shell: the control returned 2, started and exited");
    } else {
        ok = false;
        char code_text[18];
        char flags_text[18];
        char actual_text[10];
        auto _ =
            ort::debug_line({"shell: the control's record is wrong: return_code ",
                             ort::hex(static_cast<u64>(info.return_code), code_text), ", flags ",
                             ort::hex(static_cast<u64>(info.flags), flags_text), ", actual ",
                             ort::decimal(static_cast<u32>(actual), actual_text)});
    }

    // avail is written even when the record is not, so that the caller
    // learns what to ask for.
    actual = 1;
    avail = 0;
    ok &= expect("shell: object_get_info(one byte short) = ",
                 ours::object_get_info(process, InfoTopic::Process, &info, sizeof(info) - 1,
                                       &actual, &avail),
                 Status::BufferTooSmall);
    if (actual != 0 || avail != 1) {
        ok = false;
        auto _ = ort::debug_line("shell: one byte short did not write actual 0 and avail 1");
    }

    ok &= expect("shell: object_get_info(HandleBasic) = ",
                 ours::object_get_info(process, InfoTopic::HandleBasic, &info, sizeof(info),
                                       nullptr, nullptr),
                 Status::NotSupported);
    ok &= expect(
        "shell: object_get_info(a job, Process) = ",
        ours::object_get_info(job, InfoTopic::Process, &info, sizeof(info), nullptr, nullptr),
        Status::BadType);

    ours::Uandle blind = ours::Uandle::Invalid;
    constexpr auto WITHOUT_INSPECT =
        ours::Rights::Transfer | ours::Rights::Duplicate | ours::Rights::Wait;
    if (expect("shell: handle_duplicate(the control, without Inspect) = ",
               ours::handle_duplicate(process, WITHOUT_INSPECT, &blind), Status::Ok)) {
        ok &= expect(
            "shell: object_get_info(without Inspect) = ",
            ours::object_get_info(blind, InfoTopic::Process, &info, sizeof(info), nullptr, nullptr),
            Status::AccessDenied);
        auto _ = ours::handle_close(blind);
    } else {
        ok = false;
    }

    auto _ = ort::debug_line(ok ? "shell: object_get_info checks passed"
                                : "shell: object_get_info checks FAILED");
}

/// The code |process| ended with, once it has: TERMINATED first, since until
/// then a kill may still change it, then object_get_info. Either call
/// failing is reported and is the error.
auto return_code_of(ours::Uandle const process) -> ustl::Result<i64, ours::Status> {
    using ours::Status;

    if (auto const status = ours::object_wait_one(process, ours::Signals::Terminated,
                                                  ours::INSTANT_INFINITE, nullptr);
        status != Status::Ok) {
        report("shell: waiting for a probe to end = ", status);
        return ustl::err(status);
    }
    ours::InfoProcess info{};
    if (auto const status = ours::object_get_info(process, ours::InfoTopic::Process, &info,
                                                  sizeof(info), nullptr, nullptr);
        status != Status::Ok) {
        report("shell: object_get_info(a probe) = ", status);
        return ustl::err(status);
    }
    return ustl::ok(info.return_code);
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
/// Every probe is started by the launcher (U6), and "killed" is the
/// kernel's own word: the code the process ended with is
/// TASK_RETCODE_EXCEPTION_KILL, read through its handle once it has
/// terminated (return_code_of()). A probe that survives ends with SURVIVED
/// instead, and one whose image the launcher got wrong dies before main
/// with an exception like any other -- which is why the log's
/// `fault_probe: <name>` line before each kill still matters: it shows the
/// probe ran. And one more run, with a name no probe has, is the control: it
/// must answer "unknown" and return from main, the one path through which
/// the runtime ends a probe with main's own value.
///
/// The control is K23's check that a program the launcher planned, mapped, bound and started
/// reaches main and answers. It ends in a verdict of its own, "launcher
/// checks passed" or FAILED, because nothing else here would fail the boot:
/// a program the launcher got wrong dies before main, which the kernel logs
/// as a warning.
///
/// Run only by the shell userboot starts, which userboot hands the root job
/// a new process needs, and only once its launcher is set up; the dev image
/// starts the shell from init, with a channel instead, and does not package
/// fault_probe.
auto run_probes(ours::Uandle const job, shell::LaunchProgram const launcher,
                void *const launcher_context) -> void {
    constexpr std::string_view PROBES[] = {
        "pf_unmapped",   "pf_kernel",    "pf_write_vdso",     "pf_exec_vdso_data",
        "pf_write_text", "pf_exec_data", "pf_write_relro",    "ud2",
        "gp_hlt",        "gp_int80",     "gp_int3",           "de",
        "db_tf",         "gp_io",        "trap_outside_vdso", "pf_write_rodata",
    };

    // What a channel is refused for, once: a job, which Zircon would take and
    // this kernel does not yet, and a property set on an object that is not
    // an exception (the root job's handle carries SetProperty).
    auto exception_checks_ok = true;
    auto unbound = ours::Uandle::Invalid;
    exception_checks_ok &= expect(
        "shell: task_create_exception_channel(job) = ",
        ours::task_create_exception_channel(job, ours::ExceptionChannelOptions::None, &unbound),
        ours::Status::NotSupported);
    constexpr auto HANDLED = ours::ExceptionState::Handled;
    exception_checks_ok &= expect(
        "shell: object_set_property(job, ExceptionState) = ",
        ours::object_set_property(job, ours::Property::ExceptionState, &HANDLED, sizeof(HANDLED)),
        ours::Status::BadType);

    u32 killed = 0;
    u32 survived = 0;
    u32 channels_ok = 0;
    for (auto const probe: PROBES) {
        auto const run = run_probe(job, probe, launcher, launcher_context);
        if (!run.started) {
            // The control, and with it the launcher's verdict below, is never
            // reached; say so, or the launcher would go unchecked unnoticed.
            auto _ = ort::debug_line(
                "shell: launcher checks FAILED (a probe before its control did not start)");
            return;
        }
        // Each probe faults once, so its channel has to have delivered exactly
        // one exception before the probe was killed.
        if (run.exception_channel_ok && run.exceptions_seen == 1) {
            ++channels_ok;
        }
        auto const code = return_code_of(run.process);
        auto _ = ours::handle_close(run.process);
        char code_text[21];
        if (code.is_ok() && code.unwrap() == ours::TASK_RETCODE_EXCEPTION_KILL) {
            auto _ = ort::debug_line({"shell: probe ", probe, " killed (retcode ",
                                      ort::signed_decimal(code.unwrap(), code_text), ")"});
            ++killed;
        } else {
            auto const ended =
                code.is_ok() ? ort::signed_decimal(code.unwrap(), code_text) : "unknown";
            auto _ = ort::debug_line({"shell: probe ", probe, " SURVIVED (retcode ", ended,
                                      ", answered \"",
                                      std::string_view(run.answer, run.answer_size), "\")"});
            ++survived;
        }
    }

    char killed_text[10];
    char survived_text[10];
    auto _ = ort::debug_line({"shell: probes done (", ort::decimal(killed, killed_text),
                              " killed, ", ort::decimal(survived, survived_text), " survived)"});

    constexpr std::string_view CONTROL{"none"};
    auto _ = ort::debug_line("shell: starting the control probe with the launcher");
    auto const control = run_probe(job, CONTROL, launcher, launcher_context);
    auto answered_unknown = false;
    if (control.started) {
        if (control.killed) {
            auto _ = ort::debug_line(
                "shell: control probe none died without answering (expected \"unknown\")");
        } else {
            auto _ = ort::debug_line({"shell: control probe none answered \"",
                                      std::string_view(control.answer, control.answer_size), "\""});
            answered_unknown = spells(control.answer, control.answer_size, "unknown");
        }
    }

    // K23's verdict, on a line of its own: nothing above fails the boot when
    // the launcher breaks. A program it maps or binds wrongly dies before
    // main, which the kernel logs as a warning, not an error.
    if (answered_unknown) {
        auto _ = ort::debug_line("shell: launcher checks passed");
    } else {
        auto _ = ort::debug_line(
            "shell: launcher checks FAILED (its control probe did not answer \"unknown\")");
    }

    // A verdict line of its own: a channel that misbehaves fails nothing else.
    // The control probe raises no exception, so its channel delivers none.
    char channels_text[10];
    auto const channels_all = channels_ok == std::size(PROBES);
    auto const control_quiet =
        control.started && control.exception_channel_ok && control.exceptions_seen == 0;
    auto _ = ort::debug_line({"shell: exception channels delivered one exception to ",
                              ort::decimal(channels_ok, channels_text), " of the probes",
                              control_quiet ? ", none to the control" : ", and the control?"});
    if (exception_checks_ok && channels_all && control_quiet) {
        auto _ = ort::debug_line("shell: exception channel checks passed");
    } else {
        auto _ = ort::debug_line("shell: exception channel checks FAILED");
    }

    if (control.process != ours::Uandle::Invalid) {
        check_object_get_info(control.process, job);
        auto _ = ours::handle_close(control.process);
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
    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (!expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        return;
    }

    constexpr std::string_view PING{"ping"};
    ok &= expect("shell: write(ping) = ",
                 ours::channel_write(a, ours::ChannelWriteOptions::None, PING.data(),
                                     static_cast<u32>(PING.size()), nullptr, 0),
                 Status::Ok);

    // Already readable, so a poll must succeed and report Readable.
    auto observed = Signals::None;
    ok &= expect("shell: wait_one(READABLE, poll) = ",
                 ours::object_wait_one(b, Signals::Readable, 0, &observed), Status::Ok);
    ok &= !!(observed & Signals::Readable) && !!(observed & Signals::Writable);

    // Too small a buffer leaves the message where it is and says how long it
    // is.
    char buffer[16] = {};
    u32 actual = 0;
    ok &= expect("shell: read(2 bytes) = ",
                 ours::channel_read(b, ours::ChannelReadOptions::None, buffer, nullptr, 2, 0,
                                    &actual, nullptr),
                 Status::BufferTooSmall);
    ok &= actual == PING.size();

    ok &= expect("shell: read = ",
                 ours::channel_read(b, ours::ChannelReadOptions::None, buffer, nullptr,
                                    sizeof(buffer), 0, &actual, nullptr),
                 Status::Ok);
    if (spells(buffer, actual, PING)) {
        auto _ = ort::debug_line("shell: loopback ping ok");
    } else {
        ok = false;
        auto _ = ort::debug_line("shell: loopback read the wrong bytes");
    }

    ok &= expect("shell: read on empty = ",
                 ours::channel_read(b, ours::ChannelReadOptions::None, buffer, nullptr,
                                    sizeof(buffer), 0, &actual, nullptr),
                 Status::ShouldWait);

    // Closing one end tells the other: PeerClosed goes up, Writable comes
    // down, and every further read or write says so.
    ok &= expect("shell: close(a) = ", ours::handle_close(a), Status::Ok);
    ok &= expect("shell: wait_one(PEER_CLOSED) = ",
                 ours::object_wait_one(b, Signals::PeerClosed, ours::INSTANT_INFINITE, &observed),
                 Status::Ok);
    ok &= !!(observed & Signals::PeerClosed) && !(observed & Signals::Writable);
    ok &= expect("shell: read after peer closed = ",
                 ours::channel_read(b, ours::ChannelReadOptions::None, buffer, nullptr,
                                    sizeof(buffer), 0, &actual, nullptr),
                 Status::PeerClosed);
    ok &= expect("shell: write after peer closed = ",
                 ours::channel_write(b, ours::ChannelWriteOptions::None, PING.data(),
                                     static_cast<u32>(PING.size()), nullptr, 0),
                 Status::PeerClosed);
    ok &= expect("shell: close(b) = ", ours::handle_close(b), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: channel checks passed");
    } else {
        auto _ = ort::debug_line("shell: channel checks FAILED");
    }
}

/// A port (port_create, port_queue, port_wait), checked from this one
/// thread: every wait below is a poll, or one with a packet already queued.
///
/// What it checks, in the order the kernel decides it:
///   - port_queue keeps what its caller wrote -- key, status, payload -- and
///     only forces the type to User;
///   - an empty port answers a poll with TimeOut, and a deadline other than
///     0 and INSTANT_INFINITE with NotSupported;
///   - PORT_MAX_USER_PACKETS fit, and one more answers ShouldWait;
///   - a handle to something else is BadType, and a port cannot be waited on
///     (no Wait right);
///   - a port closed with packets still queued frees them. The kernel says
///     "object: port (koid N) destroyed" right after the marker line below;
///     nothing here can see that, so the boot log is the check.
auto check_port() -> void {
    using ours::PacketType;
    using ours::PortPacket;
    using ours::Status;
    using ours::Uandle;

    auto ok = true;
    Uandle port = Uandle::Invalid;
    ok &=
        expect("shell: port_create(an unknown option) = ",
               ours::port_create(static_cast<ours::PortCreateOptions>(1), &port), Status::InvArgs);
    if (!expect("shell: port_create = ", ours::port_create(ours::PortCreateOptions::None, &port),
                Status::Ok)) {
        auto _ = ort::debug_line("shell: port checks FAILED");
        return;
    }

    // SignalOne and InvArgs on the way in: the type must come back User, the
    // status as written.
    PortPacket sent{};
    sent.key = 0x5052'4F42'4500'0001;
    sent.type = PacketType::SignalOne;
    sent.status = Status::InvArgs;
    for (usize index = 0; index < sizeof(sent.user); ++index) {
        sent.user[index] = static_cast<u8>(0xA0 + index);
    }
    ok &= expect("shell: port_queue = ", ours::port_queue(port, &sent), Status::Ok);

    // Filled with 0xFF first, so a field the kernel failed to write cannot
    // pass for a right one: a zeroed type would read as User.
    PortPacket got;
    std::memset(&got, 0xFF, sizeof(got));
    ok &= expect("shell: port_wait(poll) = ", ours::port_wait(port, 0, &got), Status::Ok);
    auto const same = got.key == sent.key && got.type == PacketType::User &&
                      got.status == Status::InvArgs &&
                      std::memcmp(got.user, sent.user, sizeof(sent.user)) == 0;
    if (!same) {
        ok = false;
        auto _ = ort::debug_line("shell: port_wait read back a different packet");
    }

    ok &=
        expect("shell: port_wait(poll, empty) = ", ours::port_wait(port, 0, &got), Status::TimeOut);
    ok &= expect("shell: port_wait(deadline 5) = ", ours::port_wait(port, 5, &got),
                 Status::NotSupported);

    // Full, then one more.
    auto queued = u32{0};
    for (; queued < ours::PORT_MAX_USER_PACKETS; ++queued) {
        sent.key = queued;
        if (ours::port_queue(port, &sent) != Status::Ok) {
            break;
        }
    }
    if (queued != ours::PORT_MAX_USER_PACKETS) {
        ok = false;
        auto _ = ort::debug_line("shell: port_queue failed before the port was full");
    }
    ok &= expect("shell: port_queue(full) = ", ours::port_queue(port, &sent), Status::ShouldWait);

    // Something that is not a port, and a port that cannot be waited on.
    Uandle a = Uandle::Invalid;
    Uandle b = Uandle::Invalid;
    if (expect("shell: port channel_create = ",
               ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        ok &=
            expect("shell: port_wait(a channel) = ", ours::port_wait(a, 0, &got), Status::BadType);
        ok &= ours::handle_close(a) == Status::Ok && ours::handle_close(b) == Status::Ok;
    } else {
        ok = false;
    }
    auto observed = ours::Signals::None;
    ok &= expect("shell: wait_one(a port) = ",
                 ours::object_wait_one(port, ours::Signals::Readable, 0, &observed),
                 Status::AccessDenied);

    auto _ = ort::debug_line("shell: closing a port with 256 packets queued");
    ok &= expect("shell: close(port) = ", ours::handle_close(port), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: port checks passed");
    } else {
        auto _ = ort::debug_line("shell: port checks FAILED");
    }
}

/// object_wait_async: one-shot, level-triggered waits that deliver into a
/// port, checked from this one thread with polls only.
///
/// A channel (a, b) and a port. In order:
///   - a wait on an idle b queues nothing; a write to a queues one SignalOne
///     packet, with the signals asked for and the ones asserted, and only
///     one: the wait is spent;
///   - a wait on a b that is already readable queues its packet at once;
///   - an unknown option is refused and leaves nothing behind;
///   - closing b ends its armed waits without a packet, while a packet
///     queued before the close is still delivered;
///   - a port cannot be waited on, and the port argument must be a port;
///   - a port closed with a wait still armed lives on until the wait fires,
///     because the wait holds it; the packet is then dropped and the port
///     destroyed. The kernel's "object: port (koid N) destroyed" belongs
///     after the second of the two marker lines, not after the first; the
///     boot log is the check.
auto check_wait_async() -> void {
    using ours::PacketType;
    using ours::PortPacket;
    using ours::Signals;
    using ours::Status;
    using ours::Uandle;
    using ours::WaitAsyncOptions;

    auto ok = true;
    Uandle port = Uandle::Invalid;
    Uandle a = Uandle::Invalid;
    Uandle b = Uandle::Invalid;
    if (!expect("shell: wait_async port_create = ",
                ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok) ||
        !expect("shell: wait_async channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        auto _ = ort::debug_line("shell: wait_async checks FAILED");
        return;
    }

    // Filled with 0xFF before each wait, so that only what the kernel writes
    // can match.
    PortPacket got;
    auto const poll = [&]() -> Status {
        std::memset(&got, 0xFF, sizeof(got));
        return ours::port_wait(port, 0, &got);
    };

    ok &= expect("shell: wait_async(b, Readable) = ",
                 ours::object_wait_async(b, port, 7, Signals::Readable, WaitAsyncOptions::None),
                 Status::Ok);
    ok &= expect("shell: port_wait(nothing asserted yet) = ", poll(), Status::TimeOut);

    constexpr std::string_view PING{"ping"};
    ok &= expect("shell: wait_async write(a) = ",
                 ours::channel_write(a, ours::ChannelWriteOptions::None, PING.data(),
                                     static_cast<u32>(PING.size()), nullptr, 0),
                 Status::Ok);
    ok &= expect("shell: port_wait(after the write) = ", poll(), Status::Ok);
    auto const delivered = got.key == 7 && got.type == PacketType::SignalOne &&
                           got.status == Status::Ok && got.signal.trigger == Signals::Readable &&
                           got.signal.observed == (Signals::Readable | Signals::Writable) &&
                           got.signal.count == 1 && got.signal.timestamp == 0 &&
                           got.signal.reserved == 0;
    if (!delivered) {
        ok = false;
        auto _ = ort::debug_line("shell: wait_async delivered a different packet");
    }
    // Readable falls and rises again: a wait left armed would queue a
    // second packet now, a spent one does not.
    char drained[8] = {};
    u32 actual = 0;
    ok &= expect("shell: wait_async read(b) = ",
                 ours::channel_read(b, ours::ChannelReadOptions::None, drained, nullptr,
                                    sizeof(drained), 0, &actual, nullptr),
                 Status::Ok);
    ok &= expect("shell: wait_async write(a) again = ",
                 ours::channel_write(a, ours::ChannelWriteOptions::None, PING.data(),
                                     static_cast<u32>(PING.size()), nullptr, 0),
                 Status::Ok);
    ok &= expect("shell: port_wait(the wait is spent) = ", poll(), Status::TimeOut);

    // b is still readable: the message was never read.
    ok &= expect("shell: wait_async(b already readable) = ",
                 ours::object_wait_async(b, port, 8, Signals::Readable, WaitAsyncOptions::None),
                 Status::Ok);
    ok &= expect("shell: port_wait(queued at once) = ", poll(), Status::Ok);
    ok &= got.key == 8 && got.type == PacketType::SignalOne;

    ok &= expect(
        "shell: wait_async(an unknown option) = ",
        ours::object_wait_async(b, port, 9, Signals::Readable, static_cast<WaitAsyncOptions>(1)),
        Status::InvArgs);
    ok &= expect("shell: port_wait(after the refused wait) = ", poll(), Status::TimeOut);

    // One packet queued, one wait armed, then the handle b leaves the table:
    // sent through a second channel (x, y). The end itself lives on in the
    // message, so closing a really raises PeerClosed on it, and only the
    // cancel as b left the table stops the armed wait from firing then.
    // Reading b back from y shows the signal did go up.
    Uandle x = Uandle::Invalid;
    Uandle y = Uandle::Invalid;
    ok &= expect("shell: wait_async channel_create(x, y) = ",
                 ours::channel_create(ours::ChannelCreateOptions::None, &x, &y), Status::Ok);
    ok &= expect("shell: wait_async(b, Readable, queued) = ",
                 ours::object_wait_async(b, port, 10, Signals::Readable, WaitAsyncOptions::None),
                 Status::Ok);
    ok &= expect("shell: wait_async(b, PeerClosed, armed) = ",
                 ours::object_wait_async(b, port, 11, Signals::PeerClosed, WaitAsyncOptions::None),
                 Status::Ok);
    ok &= expect("shell: wait_async write(x, handles = [b]) = ",
                 ours::channel_write(x, ours::ChannelWriteOptions::None, PING.data(),
                                     static_cast<u32>(PING.size()), &b, 1),
                 Status::Ok);
    ok &= expect("shell: wait_async close(a) = ", ours::handle_close(a), Status::Ok);
    Uandle received = Uandle::Invalid;
    u32 received_count = 0;
    ok &= expect("shell: wait_async read(y) = ",
                 ours::channel_read(y, ours::ChannelReadOptions::None, drained, &received,
                                    sizeof(drained), 1, &actual, &received_count),
                 Status::Ok);
    auto peer_closed = Signals::None;
    ok &= received_count == 1 &&
          expect("shell: wait_one(b received, PeerClosed) = ",
                 ours::object_wait_one(received, Signals::PeerClosed, 0, &peer_closed), Status::Ok);
    ok &= expect("shell: port_wait(queued before b left) = ", poll(), Status::Ok);
    ok &= got.key == 10;
    ok &= expect("shell: port_wait(the armed wait ended with b) = ", poll(), Status::TimeOut);
    for (auto const handle: {received, x, y}) {
        ok &= ours::handle_close(handle) == Status::Ok;
    }

    ok &= expect("shell: wait_async(on a port) = ",
                 ours::object_wait_async(port, port, 12, Signals::Readable, WaitAsyncOptions::None),
                 Status::AccessDenied);
    Uandle c = Uandle::Invalid;
    Uandle d = Uandle::Invalid;
    if (expect("shell: wait_async channel_create(c, d) = ",
               ours::channel_create(ours::ChannelCreateOptions::None, &c, &d), Status::Ok)) {
        ok &= expect("shell: wait_async(port = a channel) = ",
                     ours::object_wait_async(c, d, 13, Signals::Readable, WaitAsyncOptions::None),
                     Status::BadType);
        ok &= ours::handle_close(c) == Status::Ok && ours::handle_close(d) == Status::Ok;
    } else {
        ok = false;
    }
    ok &= expect("shell: wait_async close(port) = ", ours::handle_close(port), Status::Ok);

    // A wait armed on an idle channel, then its port closed under it.
    Uandle late = Uandle::Invalid;
    if (expect("shell: wait_async port_create(late) = ",
               ours::port_create(ours::PortCreateOptions::None, &late), Status::Ok) &&
        expect("shell: wait_async channel_create(c, d) again = ",
               ours::channel_create(ours::ChannelCreateOptions::None, &c, &d), Status::Ok)) {
        ok &=
            expect("shell: wait_async(d, Readable, then the port goes) = ",
                   ours::object_wait_async(d, late, 14, Signals::Readable, WaitAsyncOptions::None),
                   Status::Ok);
        auto _ = ort::debug_line("shell: closing a port with a wait armed");
        ok &= expect("shell: wait_async close(late) = ", ours::handle_close(late), Status::Ok);
        auto _ = ort::debug_line("shell: firing the wait the closed port's packet waits on");
        ok &= expect("shell: wait_async write(c) = ",
                     ours::channel_write(c, ours::ChannelWriteOptions::None, PING.data(),
                                         static_cast<u32>(PING.size()), nullptr, 0),
                     Status::Ok);
        ok &= ours::handle_close(c) == Status::Ok && ours::handle_close(d) == Status::Ok;
    } else {
        ok = false;
    }

    if (ok) {
        auto _ = ort::debug_line("shell: wait_async checks passed");
    } else {
        auto _ = ort::debug_line("shell: wait_async checks FAILED");
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
auto check_handle_transfer() -> void {
    using ours::Status;
    using ours::Uandle;

    constexpr std::string_view PING{"ping"};

    auto ok = true;
    Uandle a = ours::Uandle::Invalid;
    Uandle b = ours::Uandle::Invalid;
    Uandle c = ours::Uandle::Invalid;
    Uandle d = ours::Uandle::Invalid;
    if (!expect("shell: transfer channel_create(a, b) = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok) ||
        !expect("shell: transfer channel_create(c, d) = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &c, &d), Status::Ok)) {
        return;
    }

    // d travels a -> b; what comes out of b is a new value for the same end.
    ok &= expect("shell: write(a, handles = [d]) = ",
                 ours::channel_write(a, ours::ChannelWriteOptions::None, nullptr, 0, &d, 1),
                 Status::Ok);
    ok &= expect("shell: close(d) after sending it = ", ours::handle_close(d), Status::BadHandle);
    Uandle got = ours::Uandle::Invalid;
    u32 actual_bytes = 0;
    u32 actual_handles = 0;
    ok &= expect("shell: read(b) = ",
                 ours::channel_read(b, ours::ChannelReadOptions::None, nullptr, &got, 0, 1,
                                    &actual_bytes, &actual_handles),
                 Status::Ok);
    ok &= actual_handles == 1 && got != ours::Uandle::Invalid;
    ok &= expect("shell: write(c, ping) = ",
                 ours::channel_write(c, ours::ChannelWriteOptions::None, PING.data(),
                                     static_cast<u32>(PING.size()), nullptr, 0),
                 Status::Ok);
    char buffer[8] = {};
    u32 length = 0;
    ok &= expect("shell: read(the received end) = ",
                 ours::channel_read(got, ours::ChannelReadOptions::None, buffer, nullptr,
                                    sizeof(buffer), 0, &length, nullptr),
                 Status::Ok);
    ok &= spells(buffer, length, PING);

    // A channel sent through itself is refused -- and consumed like any
    // other handle in the array.
    ok &= expect("shell: write(a, handles = [a]) = ",
                 ours::channel_write(a, ours::ChannelWriteOptions::None, nullptr, 0, &a, 1),
                 Status::NotSupported);
    ok &= expect("shell: close(a) after that = ", ours::handle_close(a), Status::BadHandle);

    // One bad value fails the write and takes the good handle anyway.
    Uandle e = ours::Uandle::Invalid;
    Uandle f = ours::Uandle::Invalid;
    ok &= expect("shell: transfer channel_create(e, f) = ",
                 ours::channel_create(ours::ChannelCreateOptions::None, &e, &f), Status::Ok);
    Uandle const mixed[] = {e, Uandle{0x7ffffff3}};
    ok &= expect("shell: write(c, handles = [e, bad]) = ",
                 ours::channel_write(c, ours::ChannelWriteOptions::None, nullptr, 0, mixed, 2),
                 Status::BadHandle);
    ok &= expect("shell: close(e) after that = ", ours::handle_close(e), Status::BadHandle);

    // A thousand deep: each end queued in the next channel, then only the
    // last one kept -- and closed.
    constexpr u32 DEPTH = 1000;
    auto chain_ok = true;
    Uandle inner = f;
    for (u32 i = 0; i < DEPTH && chain_ok; ++i) {
        Uandle writer = ours::Uandle::Invalid;
        Uandle holder = ours::Uandle::Invalid;
        chain_ok &=
            ours::channel_create(ours::ChannelCreateOptions::None, &writer, &holder) == Status::Ok;
        chain_ok &= ours::channel_write(writer, ours::ChannelWriteOptions::None, nullptr, 0, &inner,
                                        1) == Status::Ok;
        chain_ok &= ours::handle_close(writer) == Status::Ok;
        inner = holder;
    }
    chain_ok &= ours::handle_close(inner) == Status::Ok;
    ok &= chain_ok;
    if (chain_ok) {
        auto _ = ort::debug_line("shell: a 1000-deep chain of ends, closed");
    } else {
        auto _ = ort::debug_line("shell: the 1000-deep chain FAILED");
    }

    ok &= ours::handle_close(b) == Status::Ok;
    ok &= ours::handle_close(c) == Status::Ok;
    ok &= ours::handle_close(got) == Status::Ok;

    if (ok) {
        auto _ = ort::debug_line("shell: handle transfer checks passed");
    } else {
        auto _ = ort::debug_line("shell: handle transfer checks FAILED");
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
    using ours::Status;
    using ours::Uandle;

    // One pair more than the quota could ever allow, so that the loop ends on
    // the kernel's refusal and never on running out of room here.
    constexpr u32 ROOM = ours::HANDLE_MAX_PER_PROCESS + 2;
    static Uandle made[ROOM];
    u32 count = 0;
    auto status = Status::Ok;
    while (count + 2 <= ROOM) {
        status =
            ours::channel_create(ours::ChannelCreateOptions::None, &made[count], &made[count + 1]);
        if (status != Status::Ok) {
            break;
        }
        count += 2;
    }

    auto ok = expect("shell: channel_create past the quota = ", status, Status::NoResource);
    char count_text[10];
    auto _ = ort::debug_line(
        {"shell: ", ort::decimal(count / 2, count_text), " channels made before the quota"});

    if (count >= 2) {
        ok &= ours::handle_close(made[count - 1]) == Status::Ok;
        ok &= ours::handle_close(made[count - 2]) == Status::Ok;
        ok &= expect("shell: channel_create after closing one = ",
                     ours::channel_create(ours::ChannelCreateOptions::None, &made[count - 2],
                                          &made[count - 1]),
                     Status::Ok);
    }
    for (u32 i = 0; i < count; ++i) {
        ok &= ours::handle_close(made[i]) == Status::Ok;
    }

    if (ok) {
        auto _ = ort::debug_line("shell: handle quota checks passed");
    } else {
        auto _ = ort::debug_line("shell: handle quota checks FAILED");
    }
}

/// Two addresses no call may copy to or from, for the checks below. What each
/// one tests is in check_bad_pointers().
constexpr auto UNMAPPED = ours::VirtAddr{0x7fff'ffff'e000};
constexpr auto KERNEL = ours::VirtAddr{0xffff'8000'0000'0000};

/// Bad user pointers handed to the kernel: each call must come back InvArgs,
/// and the kernel must still be here to answer the next one.
///
/// The pointers reach the kernel exactly as written here. Three kinds:
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
                 ort::debug_write(std::string_view{reinterpret_cast<char const *>(UNMAPPED), 8}),
                 Status::InvArgs);
    ok &= expect("shell: debug_write(kernel half) = ",
                 ort::debug_write(std::string_view{reinterpret_cast<char const *>(KERNEL), 8}),
                 Status::InvArgs);

    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (!expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        return;
    }

    // The message is copied in before the channel is touched, so a bad source
    // leaves the channel as it was.
    ok &= expect("shell: channel_write(from unmapped) = ",
                 ours::channel_write(a, ours::ChannelWriteOptions::None,
                                     reinterpret_cast<void const *>(UNMAPPED), 8, nullptr, 0),
                 Status::InvArgs);
    constexpr char BYTE = 'x';
    ok &= expect("shell: channel_write(1 byte) = ",
                 ours::channel_write(a, ours::ChannelWriteOptions::None, &BYTE, 1, nullptr, 0),
                 Status::Ok);

    // Read into nothing: the message is taken off the queue before it is
    // copied out, so when the copy fails the message is gone -- the same
    // choice Zircon makes, and the next read finds the channel empty.
    u32 actual_bytes = 0;
    u32 actual_handles = 0;
    ok &= expect("shell: channel_read(into unmapped) = ",
                 ours::channel_read(b, ours::ChannelReadOptions::None,
                                    reinterpret_cast<void *>(UNMAPPED), nullptr, 1, 0,
                                    &actual_bytes, &actual_handles),
                 Status::InvArgs);
    char buffer[4] = {};
    ok &= expect("shell: channel_read(after the lost message) = ",
                 ours::channel_read(b, ours::ChannelReadOptions::None, buffer, nullptr,
                                    sizeof(buffer), 0, &actual_bytes, &actual_handles),
                 Status::ShouldWait);

    // A poll, so that the wait ends at once and the kernel goes on to write
    // |observed| -- into nothing.
    ok &= expect(
        "shell: wait_one(observed = unmapped, poll) = ",
        ours::object_wait_one(b, Signals::Readable, 0, reinterpret_cast<Signals *>(UNMAPPED)),
        Status::InvArgs);

    // Both handle values are written out before either handle is installed,
    // so the failed create leaves nothing behind and the next one works.
    ours::Uandle c = ours::Uandle::Invalid;
    ours::Uandle d = ours::Uandle::Invalid;
    ok &= expect("shell: channel_create(out0 = unmapped) = ",
                 ours::channel_create(ours::ChannelCreateOptions::None,
                                      reinterpret_cast<ours::Uandle *>(UNMAPPED), &d),
                 Status::InvArgs);
    ok &= expect("shell: channel_create(after that) = ",
                 ours::channel_create(ours::ChannelCreateOptions::None, &c, &d), Status::Ok);

    // The same three destinations, pointed at the vDSO: mapped, but not for
    // writing. The message is lost the same way, and nothing reaches the
    // vDSO -- the next call through it still works, which is the proof.
    ok &= expect("shell: channel_write(1 byte) = ",
                 ours::channel_write(a, ours::ChannelWriteOptions::None, &BYTE, 1, nullptr, 0),
                 Status::Ok);
    ok &=
        expect("shell: channel_read(into the vDSO) = ",
               ours::channel_read(b, ours::ChannelReadOptions::None, reinterpret_cast<void *>(vdso),
                                  nullptr, 1, 0, &actual_bytes, &actual_handles),
               Status::InvArgs);
    ok &= expect("shell: wait_one(observed = vDSO, poll) = ",
                 ours::object_wait_one(b, Signals::Readable, 0, reinterpret_cast<Signals *>(vdso)),
                 Status::InvArgs);
    ours::Uandle e = ours::Uandle::Invalid;
    ok &= expect("shell: channel_create(out0 = vDSO) = ",
                 ours::channel_create(ours::ChannelCreateOptions::None,
                                      reinterpret_cast<ours::Uandle *>(vdso), &e),
                 Status::InvArgs);

    ok &= expect("shell: close(a) = ", ours::handle_close(a), Status::Ok);
    ok &= expect("shell: close(b) = ", ours::handle_close(b), Status::Ok);
    ok &= expect("shell: close(c) = ", ours::handle_close(c), Status::Ok);
    ok &= expect("shell: close(d) = ", ours::handle_close(d), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: bad-pointer checks passed");
    } else {
        auto _ = ort::debug_line("shell: bad-pointer checks FAILED");
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
        auto _ = ort::debug_line("shell: libc checks passed");
    } else {
        auto _ = ort::debug_line("shell: libc checks FAILED");
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
    ok &=
        expect("shell: vmo_read(Uandle::Invalid) = ",
               ours::vmo_read(ours::Uandle::Invalid, buffer, 0, sizeof(buffer)), Status::BadHandle);

    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (!expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        return;
    }
    ok &= expect("shell: vmo_read(a channel) = ", ours::vmo_read(a, buffer, 0, sizeof(buffer)),
                 Status::BadType);
    ok &= expect("shell: vmo_read(a channel, 0 bytes) = ", ours::vmo_read(a, buffer, 0, 0),
                 Status::BadType);
    ok &= expect("shell: vmo_read(a channel, into the kernel half) = ",
                 ours::vmo_read(a, reinterpret_cast<void *>(KERNEL), 0, sizeof(buffer)),
                 Status::BadType);
    ok &= expect("shell: vmo_read(a channel, far offset) = ",
                 ours::vmo_read(a, buffer, FAR, sizeof(buffer)), Status::BadType);

    ok &= expect("shell: close(b) = ", ours::handle_close(b), Status::Ok);
    ok &= expect("shell: vmo_read(closed) = ", ours::vmo_read(b, buffer, 0, sizeof(buffer)),
                 Status::BadHandle);
    ok &= expect("shell: close(a) = ", ours::handle_close(a), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: vmo_read checks passed");
    } else {
        auto _ = ort::debug_line("shell: vmo_read checks FAILED");
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
auto check_vmo_write(ours::Uandle const bootfs) -> void {
    using ours::Status;

    constexpr auto FAR = u64{0xffff'ffff'ffff'0000};

    auto ok = true;
    char const buffer[16] = {'o', 'v', 'e', 'r', 'w', 'r', 'i', 't', 't', 'e', 'n'};
    ok &= expect("shell: vmo_write(Uandle::Invalid) = ",
                 ours::vmo_write(ours::Uandle::Invalid, buffer, 0, sizeof(buffer)),
                 Status::BadHandle);

    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (!expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        return;
    }
    ok &= expect("shell: vmo_write(a channel) = ", ours::vmo_write(a, buffer, 0, sizeof(buffer)),
                 Status::BadType);
    ok &= expect("shell: vmo_write(a channel, from the kernel half) = ",
                 ours::vmo_write(a, reinterpret_cast<void const *>(KERNEL), 0, sizeof(buffer)),
                 Status::BadType);
    ok &= expect("shell: close(b) = ", ours::handle_close(b), Status::Ok);
    ok &= expect("shell: vmo_write(closed) = ", ours::vmo_write(b, buffer, 0, sizeof(buffer)),
                 Status::BadHandle);
    ok &= expect("shell: close(a) = ", ours::handle_close(a), Status::Ok);

    if (bootfs != ours::Uandle::Invalid) {
        char before[sizeof(buffer)] = {};
        char after[sizeof(buffer)] = {};
        ok &= expect("shell: vmo_read(bootfs, before writing) = ",
                     ours::vmo_read(bootfs, before, 0, sizeof(before)), Status::Ok);
        ok &= expect("shell: vmo_write(bootfs) = ",
                     ours::vmo_write(bootfs, buffer, 0, sizeof(buffer)), Status::AccessDenied);
        ok &= expect("shell: vmo_write(bootfs, 0 bytes) = ", ours::vmo_write(bootfs, buffer, 0, 0),
                     Status::AccessDenied);
        ok &= expect(
            "shell: vmo_write(bootfs, from the kernel half, far offset) = ",
            ours::vmo_write(bootfs, reinterpret_cast<void const *>(KERNEL), FAR, sizeof(buffer)),
            Status::AccessDenied);
        ok &= expect("shell: vmo_read(bootfs, after writing) = ",
                     ours::vmo_read(bootfs, after, 0, sizeof(after)), Status::Ok);
        auto unchanged = true;
        for (usize i = 0; i < sizeof(before); ++i) {
            if (before[i] != after[i]) {
                unchanged = false;
            }
        }
        ok &= unchanged;
        if (unchanged) {
            auto _ = ort::debug_line("shell: the BootFS is as it was");
        } else {
            auto _ = ort::debug_line("shell: the BootFS CHANGED");
        }
    }

    if (ok) {
        auto _ = ort::debug_line("shell: vmo_write checks passed");
    } else {
        auto _ = ort::debug_line("shell: vmo_write checks FAILED");
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
    ours::Uandle vmo = ours::Uandle::Invalid;
    ok &= expect("shell: vmo_create(Resizable) = ",
                 ours::vmo_create(PAGE, VmoOptions::Resizable, &vmo), Status::NotSupported);
    ok &= expect("shell: vmo_create(an unknown option) = ",
                 ours::vmo_create(PAGE, static_cast<VmoOptions>(1U << 5), &vmo), Status::InvArgs);
    ok &= expect("shell: vmo_create(Resizable and an unknown option) = ",
                 ours::vmo_create(PAGE, static_cast<VmoOptions>((1U << 1) | (1U << 5)), &vmo),
                 Status::InvArgs);
    ok &= expect("shell: vmo_create(the largest u64) = ",
                 ours::vmo_create(~u64{0}, VmoOptions::None, &vmo), Status::OutOfRange);
    ok &= expect("shell: vmo_create(out = kernel half) = ",
                 ours::vmo_create(PAGE, VmoOptions::None, reinterpret_cast<ours::Uandle *>(KERNEL)),
                 Status::InvArgs);

    // Empty: valid, and every non-empty range is past its end.
    ours::Uandle empty = ours::Uandle::Invalid;
    if (expect("shell: vmo_create(0 bytes) = ", ours::vmo_create(0, VmoOptions::None, &empty),
               Status::Ok)) {
        u8 byte = 0;
        ok &= expect("shell: vmo_read(empty, 0 bytes) = ", ours::vmo_read(empty, &byte, 0, 0),
                     Status::Ok);
        ok &= expect("shell: vmo_write(empty, 1 byte) = ", ours::vmo_write(empty, &byte, 0, 1),
                     Status::OutOfRange);
        ok &= expect("shell: close(empty) = ", ours::handle_close(empty), Status::Ok);
    } else {
        ok = false;
    }

    // 3 pages and a byte: rounded up to 4.
    if (!expect("shell: vmo_create(3 pages + 1 byte) = ",
                ours::vmo_create(3 * PAGE + 1, VmoOptions::None, &vmo), Status::Ok)) {
        auto _ = ort::debug_line("shell: vmo_create checks FAILED");
        return;
    }
    u8 tail[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    ok &= expect("shell: vmo_read(the last bytes of page 4) = ",
                 ours::vmo_read(vmo, tail, 4 * PAGE - sizeof(tail), sizeof(tail)), Status::Ok);
    auto zero = true;
    for (auto const b: tail) {
        if (b != 0) {
            zero = false;
        }
    }
    ok &= zero;
    if (zero) {
        auto _ = ort::debug_line("shell: an unwritten page reads as zero");
    }
    ok &= expect("shell: vmo_read(one byte past page 4) = ", ours::vmo_read(vmo, tail, 4 * PAGE, 1),
                 Status::OutOfRange);

    // A write across the first page boundary, read back.
    u8 const written[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    u8 back[16] = {};
    ok &= expect("shell: vmo_write(across a page boundary) = ",
                 ours::vmo_write(vmo, written, PAGE - 8, sizeof(written)), Status::Ok);
    ok &= expect("shell: vmo_read(it back) = ", ours::vmo_read(vmo, back, PAGE - 8, sizeof(back)),
                 Status::Ok);
    auto same = true;
    for (usize i = 0; i < sizeof(written); ++i) {
        if (written[i] != back[i]) {
            same = false;
        }
    }
    ok &= same;
    if (same) {
        auto _ = ort::debug_line("shell: vmo_write reads back the same");
    }
    ok &= expect("shell: vmo_write(offset = size, 1 byte) = ",
                 ours::vmo_write(vmo, written, 4 * PAGE, 1), Status::OutOfRange);
    ok &= expect("shell: vmo_write(size + 1 bytes) = ",
                 ours::vmo_write(vmo, written, 0, 4 * PAGE + 1), Status::OutOfRange);
    ok &=
        expect("shell: vmo_write(from the kernel half) = ",
               ours::vmo_write(vmo, reinterpret_cast<void const *>(KERNEL), 0, 8), Status::InvArgs);

    // A copy that may read but not write.
    ours::Uandle readonly = ours::Uandle::Invalid;
    if (expect("shell: handle_duplicate(vmo, without Write) = ",
               ours::handle_duplicate(vmo, Rights::Basic | Rights::Read | Rights::Map, &readonly),
               Status::Ok)) {
        ok &= expect("shell: vmo_write(without Write) = ",
                     ours::vmo_write(readonly, written, 0, sizeof(written)), Status::AccessDenied);
        ok &= expect("shell: close(readonly) = ", ours::handle_close(readonly), Status::Ok);
    } else {
        ok = false;
    }
    ok &= expect("shell: close(vmo) = ", ours::handle_close(vmo), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: vmo_create checks passed");
    } else {
        auto _ = ort::debug_line("shell: vmo_create checks FAILED");
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
auto check_bootfs(ours::Uandle const bootfs, ours::VirtAddr const vdso) -> void {
    using ours::Status;

    if (bootfs == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: bootfs checks skipped (no BootFS here)");
        return;
    }

    auto ok = true;
    bootfs::Header header{};
    ok &= expect("shell: vmo_read(header) = ", ours::vmo_read(bootfs, &header, 0, sizeof(header)),
                 Status::Ok);

    // The entry table and the names, into fixed buffers: this image's BootFS
    // has four files, and a much bigger one is not what this check is for.
    constexpr u32 MAX_ENTRIES = 16;
    constexpr u32 MAX_NAMES = 512;
    bootfs::Entry entries[MAX_ENTRIES];
    char names[MAX_NAMES];
    if (!ok || header.file_count == 0 || header.file_count > MAX_ENTRIES ||
        header.name_table_size > MAX_NAMES) {
        auto _ = ort::debug_line("shell: bootfs checks FAILED (header)");
        return;
    }
    auto const entries_size = usize{header.file_count} * sizeof(bootfs::Entry);
    ok &= expect("shell: vmo_read(entries) = ",
                 ours::vmo_read(bootfs, entries, sizeof(header), entries_size), Status::Ok);
    ok &=
        expect("shell: vmo_read(names) = ",
               ours::vmo_read(bootfs, names, sizeof(header) + entries_size, header.name_table_size),
               Status::Ok);

    for (u32 i = 0; ok && i < header.file_count; ++i) {
        auto const &entry = entries[i];
        if (u64{entry.name_offset} + entry.name_size > header.name_table_size) {
            ok = false;
            break;
        }
        auto const name = std::string_view(names + entry.name_offset, entry.name_size);
        char size_text[10];
        auto _ = ort::debug_line({"shell: bootfs entry ", name, " (",
                                  ort::decimal(static_cast<u32>(entry.data_size), size_text),
                                  " bytes)"});

        // The kernel's own files, wherever they sit: the names
        // KernelPackage gives a kernel image and its patches.
        auto base = name;
        for (usize at = name.size(); at > 0; --at) {
            if (name.data()[at - 1] == '/') {
                base = std::string_view(name.data() + at, name.size() - at);
                break;
            }
        }
        if (spells(base.data(), static_cast<u32>(base.size()), "image.elf") ||
            spells(base.data(), static_cast<u32>(base.size()), "code-patches")) {
            auto _ = ort::debug_line({"shell: bootfs holds a kernel file: ", name});
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
        auto _ = ort::debug_line("shell: no BootFS file spans two pages");
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
                     ours::vmo_read(bootfs, across, boundary - 8, sizeof(across)), Status::Ok);
        ok &= expect("shell: vmo_read(the page before) = ",
                     ours::vmo_read(bootfs, before, boundary - 8, sizeof(before)), Status::Ok);
        ok &= expect("shell: vmo_read(the page after) = ",
                     ours::vmo_read(bootfs, after, boundary, sizeof(after)), Status::Ok);
        ok &= same_bytes(across, before, sizeof(before)) &&
              same_bytes(across + sizeof(before), after, sizeof(after));
    }

    // The edges of the range: empty at the very end is in range, a byte
    // there is not, and neither is one byte more than the whole VMO.
    auto const size = header.total_size;
    u8 byte = 0;
    ok &=
        expect("shell: vmo_read(offset = size, 0 bytes) = ", ours::vmo_read(bootfs, &byte, size, 0),
               Status::Ok);
    ok &=
        expect("shell: vmo_read(offset = size, 1 byte) = ", ours::vmo_read(bootfs, &byte, size, 1),
               Status::OutOfRange);
    ok &=
        expect("shell: vmo_read(size + 1 bytes) = ",
               ours::vmo_read(bootfs, &byte, 0, static_cast<usize>(size) + 1), Status::OutOfRange);

    // Destinations the kernel may not write: the kernel half (refused before
    // any page is read), nothing mapped, and the read-only vDSO (both found
    // while copying).
    ok &= expect("shell: vmo_read(into the kernel half) = ",
                 ours::vmo_read(bootfs, reinterpret_cast<void *>(KERNEL), 0, 8), Status::InvArgs);
    ok &= expect("shell: vmo_read(into unmapped) = ",
                 ours::vmo_read(bootfs, reinterpret_cast<void *>(UNMAPPED), 0, 8), Status::InvArgs);
    ok &= expect("shell: vmo_read(into the vDSO) = ",
                 ours::vmo_read(bootfs, reinterpret_cast<void *>(vdso), 0, 8), Status::InvArgs);

    if (ok) {
        auto _ = ort::debug_line("shell: bootfs checks passed");
    } else {
        auto _ = ort::debug_line("shell: bootfs checks FAILED");
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
auto check_handle_duplicate(ours::Uandle const bootfs) -> void {
    using ours::Rights;
    using ours::Status;

    auto ok = true;
    ours::Uandle copy = ours::Uandle::Invalid;
    ok &= expect("shell: handle_duplicate(Uandle::Invalid) = ",
                 ours::handle_duplicate(ours::Uandle::Invalid, Rights::SameRights, &copy),
                 Status::BadHandle);

    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (!expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        return;
    }
    ok &= expect("shell: handle_duplicate(a channel) = ",
                 ours::handle_duplicate(a, Rights::SameRights, &copy), Status::AccessDenied);
    ok &= expect("shell: close(a) = ", ours::handle_close(a), Status::Ok);
    ok &= expect("shell: close(b) = ", ours::handle_close(b), Status::Ok);

    if (bootfs == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: handle_duplicate on a VMO skipped (no BootFS)");
    } else {
        // Same rights: both read the same header.
        ok &= expect("shell: handle_duplicate(bootfs, SameRights) = ",
                     ours::handle_duplicate(bootfs, Rights::SameRights, &copy), Status::Ok);
        bootfs::Header original{};
        bootfs::Header copied{};
        ok &= expect("shell: vmo_read(original) = ",
                     ours::vmo_read(bootfs, &original, 0, sizeof(original)), Status::Ok);
        ok &= expect("shell: vmo_read(copy) = ", ours::vmo_read(copy, &copied, 0, sizeof(copied)),
                     Status::Ok);
        ok &= copy != bootfs && same_bytes(&original, &copied, sizeof(original)) &&
              original.magic == bootfs::Header::MAGIC;
        ok &= expect("shell: close(copy) = ", ours::handle_close(copy), Status::Ok);

        // Never wider: the BootFS is handed out without Write, and bit 31
        // beside another right is not "the same rights".
        ok &= expect("shell: handle_duplicate(bootfs, Write) = ",
                     ours::handle_duplicate(bootfs, Rights::Write, &copy), Status::InvArgs);
        ok &= expect("shell: handle_duplicate(bootfs, SameRights | Read) = ",
                     ours::handle_duplicate(bootfs, Rights::SameRights | Rights::Read, &copy),
                     Status::InvArgs);

        // Narrower: without Read, the copy cannot be read through.
        ours::Uandle unreadable = ours::Uandle::Invalid;
        ok &= expect("shell: handle_duplicate(bootfs, without Read) = ",
                     ours::handle_duplicate(bootfs, Rights::Basic | Rights::Map, &unreadable),
                     Status::Ok);
        u8 byte = 0;
        ok &= expect("shell: vmo_read(the copy without Read) = ",
                     ours::vmo_read(unreadable, &byte, 0, 1), Status::AccessDenied);
        ok &= expect("shell: close(the copy without Read) = ", ours::handle_close(unreadable),
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
            leaked =
                ours::handle_duplicate(bootfs, Rights::SameRights,
                                       reinterpret_cast<ours::Uandle *>(KERNEL)) != Status::InvArgs;
        }
        ok &= expect("shell: handle_duplicate(out = kernel half), past the quota = ",
                     leaked ? Status::InternalError : Status::InvArgs, Status::InvArgs);
        ok &= expect("shell: handle_duplicate(bootfs) after that = ",
                     ours::handle_duplicate(bootfs, Rights::SameRights, &copy), Status::Ok);
        ok &= expect("shell: close(copy) = ", ours::handle_close(copy), Status::Ok);
    }

    if (ok) {
        auto _ = ort::debug_line("shell: handle_duplicate checks passed");
    } else {
        auto _ = ort::debug_line("shell: handle_duplicate checks FAILED");
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
///     are InvArgs; Reference, and Slice with NoWrite beside it, NotSupported;
///     an unknown bit InvArgs;
///   - the handle: a channel is BadType, one without Read AccessDenied.
/// What needs the BootFS runs where userboot started this program: a slice of
/// the `vdso` entry starts with the ELF magic, and inherits the BootFS
/// handle's lack of Write.
auto check_vmo_create_child(ours::Uandle const bootfs) -> void {
    using ours::Rights;
    using ours::Status;
    using ours::VmoChildOptions;
    using ours::VmoOptions;

    constexpr auto PAGE = u64{4096};

    auto ok = true;
    ours::Uandle parent = ours::Uandle::Invalid;
    if (!expect("shell: vmo_create(3 pages) = ",
                ours::vmo_create(3 * PAGE, VmoOptions::None, &parent), Status::Ok)) {
        auto _ = ort::debug_line("shell: vmo_create_child checks FAILED");
        return;
    }

    // Parent to slice.
    u8 const down[4] = {0xd0, 0xd1, 0xd2, 0xd3};
    u8 got[4] = {};
    ok &= expect("shell: vmo_write(parent, page 2) = ",
                 ours::vmo_write(parent, down, PAGE, sizeof(down)), Status::Ok);
    ours::Uandle slice = ours::Uandle::Invalid;
    ok &= expect("shell: vmo_create_child(Slice, page 2) = ",
                 ours::vmo_create_child(parent, VmoChildOptions::Slice, PAGE, PAGE, &slice),
                 Status::Ok);
    ok &= expect("shell: vmo_read(slice, 0) = ", ours::vmo_read(slice, got, 0, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(down, got, sizeof(got));

    // Slice to parent: the slice commits the page the parent reads.
    u8 const up[4] = {0xa0, 0xa1, 0xa2, 0xa3};
    ok &= expect("shell: vmo_write(slice, 8) = ", ours::vmo_write(slice, up, 8, sizeof(up)),
                 Status::Ok);
    ok &= expect("shell: vmo_read(parent, page 2 + 8) = ",
                 ours::vmo_read(parent, got, PAGE + 8, sizeof(got)), Status::Ok);
    ok &= same_bytes(up, got, sizeof(got));

    // The window's end, which the parent's pages do not stop at.
    ok &= expect("shell: vmo_read(slice, one byte past its end) = ",
                 ours::vmo_read(slice, got, PAGE, 1), Status::OutOfRange);

    // A slice of a slice: the parent's pages 2..3, then page 1 of that.
    ours::Uandle wide = ours::Uandle::Invalid;
    ours::Uandle nested = ours::Uandle::Invalid;
    ok &= expect("shell: vmo_create_child(Slice, pages 2-3) = ",
                 ours::vmo_create_child(parent, VmoChildOptions::Slice, PAGE, 2 * PAGE, &wide),
                 Status::Ok);
    ok &= expect("shell: vmo_create_child(Slice of it, its page 1) = ",
                 ours::vmo_create_child(wide, VmoChildOptions::Slice, 0, 1, &nested), Status::Ok);
    ok &= expect("shell: vmo_read(nested, 8) = ", ours::vmo_read(nested, got, 8, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(up, got, sizeof(got));
    // A size of 1 was a page: its last byte is there, the next is not.
    ok &= expect("shell: vmo_read(nested, last byte of the page) = ",
                 ours::vmo_read(nested, got, PAGE - 1, 1), Status::Ok);
    ok &= expect("shell: vmo_read(nested, one past) = ", ours::vmo_read(nested, got, PAGE, 1),
                 Status::OutOfRange);
    ok &= expect("shell: close(nested) = ", ours::handle_close(nested), Status::Ok);
    ok &= expect("shell: close(wide) = ", ours::handle_close(wide), Status::Ok);

    // The arguments.
    ours::Uandle refused = ours::Uandle::Invalid;
    ok &= expect("shell: vmo_create_child(unaligned offset) = ",
                 ours::vmo_create_child(parent, VmoChildOptions::Slice, 8, PAGE, &refused),
                 Status::InvArgs);
    ok &=
        expect("shell: vmo_create_child(past the end) = ",
               ours::vmo_create_child(parent, VmoChildOptions::Slice, 2 * PAGE, 2 * PAGE, &refused),
               Status::InvArgs);
    ok &= expect("shell: vmo_create_child(Reference) = ",
                 ours::vmo_create_child(parent, VmoChildOptions::Reference, 0, 0, &refused),
                 Status::NotSupported);
    ok &= expect("shell: vmo_create_child(SnapshotFull | SnapshotOnWrite) = ",
                 ours::vmo_create_child(parent,
                                        static_cast<VmoChildOptions>(
                                            std::to_underlying(VmoChildOptions::SnapshotFull) |
                                            std::to_underlying(VmoChildOptions::SnapshotOnWrite)),
                                        0, PAGE, &refused),
                 Status::NotSupported);
    ok &= expect("shell: vmo_create_child(Slice | NoWrite) = ",
                 ours::vmo_create_child(
                     parent,
                     static_cast<VmoChildOptions>(std::to_underlying(VmoChildOptions::Slice) |
                                                  std::to_underlying(VmoChildOptions::NoWrite)),
                     0, PAGE, &refused),
                 Status::NotSupported);
    ok &= expect(
        "shell: vmo_create_child(an unknown bit) = ",
        ours::vmo_create_child(parent, static_cast<VmoChildOptions>(1U << 9), 0, PAGE, &refused),
        Status::InvArgs);

    // The handle.
    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (expect("shell: channel_create = ",
               ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        ok &= expect("shell: vmo_create_child(a channel) = ",
                     ours::vmo_create_child(a, VmoChildOptions::Slice, 0, PAGE, &refused),
                     Status::BadType);
        ok &= expect("shell: close(a) = ", ours::handle_close(a), Status::Ok);
        ok &= expect("shell: close(b) = ", ours::handle_close(b), Status::Ok);
    } else {
        ok = false;
    }
    ours::Uandle unreadable = ours::Uandle::Invalid;
    ok &= expect("shell: handle_duplicate(parent, without Read) = ",
                 ours::handle_duplicate(parent, Rights::Basic | Rights::Map, &unreadable),
                 Status::Ok);
    ok &= expect("shell: vmo_create_child(without Read) = ",
                 ours::vmo_create_child(unreadable, VmoChildOptions::Slice, 0, PAGE, &refused),
                 Status::AccessDenied);
    ok &= expect("shell: close(the copy without Read) = ", ours::handle_close(unreadable),
                 Status::Ok);

    // Snapshots: a copy-on-write clone. Neither side sees the other's writes after
    // it, and pages past the parent's end read as zero.
    ours::Uandle source = ours::Uandle::Invalid;
    ours::Uandle full = ours::Uandle::Invalid;
    ours::Uandle on_write = ours::Uandle::Invalid;
    u8 const before[4] = {0x5a, 0x5b, 0x5c, 0x5d};
    u8 const theirs[4] = {0x6a, 0x6b, 0x6c, 0x6d};
    u8 const mine[4] = {0x7a, 0x7b, 0x7c, 0x7d};
    u8 const zero[4] = {};
    ok &= expect("shell: vmo_create(snapshot source, 2 pages) = ",
                 ours::vmo_create(2 * PAGE, VmoOptions::None, &source), Status::Ok);
    ok &=
        expect("shell: vmo_write(source, 0) = ", ours::vmo_write(source, before, 0, sizeof(before)),
               Status::Ok);
    ok &= expect("shell: vmo_create_child(SnapshotFull, 3 pages) = ",
                 ours::vmo_create_child(source, VmoChildOptions::SnapshotFull, 0, 3 * PAGE, &full),
                 Status::Ok);
    ok &= expect("shell: vmo_read(snapshot, 0) = ", ours::vmo_read(full, got, 0, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(before, got, sizeof(got));
    // The snapshot writes its page 0: the source still has its own.
    ok &= expect("shell: vmo_write(snapshot, 0) = ", ours::vmo_write(full, mine, 0, sizeof(mine)),
                 Status::Ok);
    ok &= expect("shell: vmo_read(source, 0) = ", ours::vmo_read(source, got, 0, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(before, got, sizeof(got));
    // The source writes its pages 0 and 1: the snapshot sees neither.
    ok &= expect("shell: vmo_write(source, 0) again = ",
                 ours::vmo_write(source, theirs, 0, sizeof(theirs)), Status::Ok);
    ok &= expect("shell: vmo_write(source, page 1) = ",
                 ours::vmo_write(source, theirs, PAGE, sizeof(theirs)), Status::Ok);
    ok &= expect("shell: vmo_read(snapshot, 0) again = ", ours::vmo_read(full, got, 0, sizeof(got)),
                 Status::Ok);
    ok &= same_bytes(mine, got, sizeof(got));
    ok &=
        expect("shell: vmo_read(snapshot, page 1) = ", ours::vmo_read(full, got, PAGE, sizeof(got)),
               Status::Ok);
    ok &= same_bytes(zero, got, sizeof(got));
    // Past the source's end.
    ok &= expect("shell: vmo_read(snapshot, page 2) = ",
                 ours::vmo_read(full, got, 2 * PAGE, sizeof(got)), Status::Ok);
    ok &= same_bytes(zero, got, sizeof(got));
    // SnapshotOnWrite: a snapshot of the source as it is now.
    ok &=
        expect("shell: vmo_create_child(SnapshotOnWrite) = ",
               ours::vmo_create_child(source, VmoChildOptions::SnapshotOnWrite, 0, PAGE, &on_write),
               Status::Ok);
    ok &= expect("shell: vmo_read(on-write snapshot, 0) = ",
                 ours::vmo_read(on_write, got, 0, sizeof(got)), Status::Ok);
    ok &= same_bytes(theirs, got, sizeof(got));
    ok &= expect("shell: vmo_create_child(SnapshotFull, unaligned offset) = ",
                 ours::vmo_create_child(source, VmoChildOptions::SnapshotFull, 8, PAGE, &refused),
                 Status::InvArgs);
    // A snapshot of a handle without Write can be written, and what it writes
    // stays its own: unlike a slice, it has no way to the source's pages.
    ours::Uandle readonly = ours::Uandle::Invalid;
    ours::Uandle writable = ours::Uandle::Invalid;
    ok &= expect(
        "shell: handle_duplicate(source, without Write) = ",
        ours::handle_duplicate(source, Rights::Basic | Rights::Read | Rights::Map, &readonly),
        Status::Ok);
    ok &= expect("shell: vmo_write(source, through the read-only copy) = ",
                 ours::vmo_write(readonly, mine, 0, sizeof(mine)), Status::AccessDenied);
    ok &=
        expect("shell: vmo_create_child(the read-only copy, SnapshotFull) = ",
               ours::vmo_create_child(readonly, VmoChildOptions::SnapshotFull, 0, PAGE, &writable),
               Status::Ok);
    ok &= expect("shell: vmo_write(a snapshot of the read-only copy) = ",
                 ours::vmo_write(writable, mine, 0, sizeof(mine)), Status::Ok);
    ok &= expect("shell: vmo_read(source, 0) after it = ",
                 ours::vmo_read(source, got, 0, sizeof(got)), Status::Ok);
    ok &= same_bytes(theirs, got, sizeof(got));
    ok &=
        expect("shell: close(the writable snapshot) = ", ours::handle_close(writable), Status::Ok);
    // SnapshotModified, of an anonymous VMO, is a full snapshot: it gets Write
    // as the others do, and does not see the source's later writes.
    ours::Uandle modified = ours::Uandle::Invalid;
    ok &= expect(
        "shell: vmo_create_child(the read-only copy, SnapshotModified) = ",
        ours::vmo_create_child(readonly, VmoChildOptions::SnapshotModified, 0, PAGE, &modified),
        Status::Ok);
    ok &= expect("shell: vmo_write(source, 0) after SnapshotModified = ",
                 ours::vmo_write(source, before, 0, sizeof(before)), Status::Ok);
    ok &= expect("shell: vmo_read(modified snapshot, 0) = ",
                 ours::vmo_read(modified, got, 0, sizeof(got)), Status::Ok);
    ok &= same_bytes(theirs, got, sizeof(got));
    ok &= expect("shell: vmo_write(modified snapshot, 0) = ",
                 ours::vmo_write(modified, mine, 0, sizeof(mine)), Status::Ok);
    ok &= expect("shell: close(modified snapshot) = ", ours::handle_close(modified), Status::Ok);
    ok &= expect("shell: close(the read-only copy) = ", ours::handle_close(readonly), Status::Ok);
    ok &= expect("shell: close(on-write snapshot) = ", ours::handle_close(on_write), Status::Ok);
    ok &= expect("shell: close(snapshot) = ", ours::handle_close(full), Status::Ok);
    ok &= expect("shell: close(snapshot source) = ", ours::handle_close(source), Status::Ok);

    ok &= expect("shell: close(slice) = ", ours::handle_close(slice), Status::Ok);
    ok &= expect("shell: close(parent) = ", ours::handle_close(parent), Status::Ok);

    if (bootfs == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vmo_create_child on the BootFS skipped");
    } else {
        // The `vdso` entry, found the way check_bootfs() reads the directory.
        bootfs::Header header{};
        constexpr u32 MAX_ENTRIES = 16;
        constexpr u32 MAX_NAMES = 512;
        bootfs::Entry entries[MAX_ENTRIES];
        char names[MAX_NAMES];
        auto found = false;
        ok &=
            expect("shell: vmo_read(header) = ", ours::vmo_read(bootfs, &header, 0, sizeof(header)),
                   Status::Ok);
        if (header.file_count <= MAX_ENTRIES && header.name_table_size <= MAX_NAMES) {
            auto const entries_size = usize{header.file_count} * sizeof(bootfs::Entry);
            ok &= expect("shell: vmo_read(entries) = ",
                         ours::vmo_read(bootfs, entries, sizeof(header), entries_size), Status::Ok);
            ok &= expect("shell: vmo_read(names) = ",
                         ours::vmo_read(bootfs, names, sizeof(header) + entries_size,
                                        header.name_table_size),
                         Status::Ok);
            for (u32 i = 0; ok && !found && i < header.file_count; ++i) {
                auto const &entry = entries[i];
                if (u64{entry.name_offset} + entry.name_size > header.name_table_size ||
                    !spells(names + entry.name_offset, entry.name_size, "vdso")) {
                    continue;
                }
                found = true;
                ours::Uandle vdso = ours::Uandle::Invalid;
                ok &= expect("shell: vmo_create_child(bootfs, Slice, vdso) = ",
                             ours::vmo_create_child(bootfs, VmoChildOptions::Slice,
                                                    entry.data_offset, entry.data_size, &vdso),
                             Status::Ok);
                char magic[4] = {};
                ok &= expect("shell: vmo_read(the vdso slice, 0) = ",
                             ours::vmo_read(vdso, magic, 0, sizeof(magic)), Status::Ok);
                ok &= spells(magic, sizeof(magic),
                             "\x7f"
                             "ELF");
                ok &= expect("shell: vmo_write(the vdso slice) = ",
                             ours::vmo_write(vdso, magic, 0, 1), Status::AccessDenied);
                ok &=
                    expect("shell: close(the vdso slice) = ", ours::handle_close(vdso), Status::Ok);

                // A snapshot of the same entry can be written, though the BootFS
                // cannot, and what it writes stays its own.
                ours::Uandle copy = ours::Uandle::Invalid;
                char const mark[1] = {'X'};
                ok &= expect("shell: vmo_create_child(bootfs, SnapshotFull, vdso) = ",
                             ours::vmo_create_child(bootfs, VmoChildOptions::SnapshotFull,
                                                    entry.data_offset, entry.data_size, &copy),
                             Status::Ok);
                ok &= expect("shell: vmo_write(the vdso snapshot) = ",
                             ours::vmo_write(copy, mark, 0, sizeof(mark)), Status::Ok);
                ok &= expect("shell: vmo_read(the vdso snapshot, 0) = ",
                             ours::vmo_read(copy, magic, 0, sizeof(magic)), Status::Ok);
                ok &= magic[0] == 'X';
                ok &= expect("shell: vmo_read(bootfs, the vdso, 0) = ",
                             ours::vmo_read(bootfs, magic, entry.data_offset, sizeof(magic)),
                             Status::Ok);
                ok &= spells(magic, sizeof(magic),
                             "\x7f"
                             "ELF");
                ok &= expect("shell: close(the vdso snapshot) = ", ours::handle_close(copy),
                             Status::Ok);
            }
        }
        if (!found) {
            auto _ = ort::debug_line("shell: no vdso entry in the BootFS");
            ok = false;
        }
    }

    if (ok) {
        auto _ = ort::debug_line("shell: vmo_create_child checks passed");
    } else {
        auto _ = ort::debug_line("shell: vmo_create_child checks FAILED");
    }
}

/// vmo_replace_as_executable: vmex turns a VMO handle into one that may be
/// executed, and the handle it is given is consumed whatever the answer (K19).
///
/// What needs no vmex runs in every image, each on a VMO of its own so that
/// "the original is gone" can be checked after every call:
///   - vmex = Uandle::Invalid is AccessDenied, and the VMO handle is gone;
///   - vmex a channel is BadType; vmex a value that names nothing, BadHandle;
///   - a VMO handle that is a channel is BadHandle, and the channel is gone.
/// What needs vmex runs where userboot started this program: the BootFS as
/// vmex is BadType (a VMO, not a resource), and the real vmex gives a handle
/// that reads what the original wrote, cannot be written through (Write is
/// dropped, design D3), and leaves the original's value naming nothing.
auto check_vmo_replace_as_executable(ours::Uandle const bootfs, ours::Uandle const vmex) -> void {
    using ours::Status;
    using ours::VmoOptions;

    constexpr auto PAGE = u64{4096};

    auto ok = true;
    ours::Uandle exec = ours::Uandle::Invalid;
    u8 byte = 0;

    // A fresh VMO for each refusal: every one of them consumes it.
    auto const refused = [&](std::string_view const what, ours::Uandle const with,
                             Status const want) {
        ours::Uandle vmo = ours::Uandle::Invalid;
        if (!expect("shell: vmo_create = ", ours::vmo_create(PAGE, VmoOptions::None, &vmo),
                    Status::Ok)) {
            return false;
        }
        auto passed = expect(what, ours::vmo_replace_as_executable(vmo, with, &exec), want);
        passed &=
            expect("shell: vmo_read(the consumed handle) = ", ours::vmo_read(vmo, &byte, 0, 1),
                   Status::BadHandle);
        return passed;
    };

    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (!expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        return;
    }
    ok &= refused("shell: vmo_replace_as_executable(vmex = Uandle::Invalid) = ",
                  ours::Uandle::Invalid, Status::AccessDenied);
    ok &= refused("shell: vmo_replace_as_executable(vmex = a channel) = ", a, Status::BadType);

    // A value that names nothing: b's, once b is closed.
    auto const stale = b;
    ok &= expect("shell: close(b) = ", ours::handle_close(b), Status::Ok);
    ok &= refused("shell: vmo_replace_as_executable(vmex = a closed handle) = ", stale,
                  Status::BadHandle);

    // Not a VMO: refused, and consumed all the same.
    ok &= expect("shell: vmo_replace_as_executable(vmo = a channel) = ",
                 ours::vmo_replace_as_executable(a, vmex, &exec), Status::BadHandle);
    ok &= expect("shell: close(the consumed channel) = ", ours::handle_close(a), Status::BadHandle);

    if (vmex == ours::Uandle::Invalid || bootfs == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vmo_replace_as_executable with vmex skipped");
    } else {
        ok &= refused("shell: vmo_replace_as_executable(vmex = the BootFS) = ", bootfs,
                      Status::BadType);

        ours::Uandle vmo = ours::Uandle::Invalid;
        if (expect("shell: vmo_create = ", ours::vmo_create(PAGE, VmoOptions::None, &vmo),
                   Status::Ok)) {
            u8 const written = 0x5a;
            ok &= expect("shell: vmo_write(before the replace) = ",
                         ours::vmo_write(vmo, &written, 0, 1), Status::Ok);
            ok &= expect("shell: vmo_replace_as_executable(vmex) = ",
                         ours::vmo_replace_as_executable(vmo, vmex, &exec), Status::Ok);
            ok &= expect("shell: vmo_read(the original) = ", ours::vmo_read(vmo, &byte, 0, 1),
                         Status::BadHandle);
            ok &= expect("shell: vmo_read(the executable handle) = ",
                         ours::vmo_read(exec, &byte, 0, 1), Status::Ok);
            ok &= byte == written;
            ok &= expect("shell: vmo_write(the executable handle) = ",
                         ours::vmo_write(exec, &written, 0, 1), Status::AccessDenied);
            ok &= expect("shell: close(the executable handle) = ", ours::handle_close(exec),
                         Status::Ok);
        } else {
            ok = false;
        }
    }

    if (ok) {
        auto _ = ort::debug_line("shell: vmo_replace_as_executable checks passed");
    } else {
        auto _ = ort::debug_line("shell: vmo_replace_as_executable checks FAILED");
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
auto check_vma_map(ours::Uandle const vma, ours::Uandle const bootfs, ours::Uandle const vmex)
    -> void {
    using ours::Rights;
    using ours::Status;
    using ours::VmoOptions;
    using ours::VmOptions;

    constexpr auto PAGE = u64{4096};
    constexpr auto R = VmOptions::PermRead;
    constexpr auto RW = VmOptions::PermRead | VmOptions::PermWrite;
    constexpr auto RX = VmOptions::PermRead | VmOptions::PermExecute;

    if (vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vma_map checks FAILED (no VmaRoot)");
        return;
    }

    auto ok = true;
    ours::Uandle vmo = ours::Uandle::Invalid;
    if (!expect("shell: vmo_create(2 pages) = ", ours::vmo_create(2 * PAGE, VmoOptions::None, &vmo),
                Status::Ok)) {
        auto _ = ort::debug_line("shell: vma_map checks FAILED");
        return;
    }
    u8 const second[4] = {0x21, 0x22, 0x23, 0x24};
    ok &= expect("shell: vmo_write(page 2) = ", ours::vmo_write(vmo, second, PAGE, sizeof(second)),
                 Status::Ok);

    // Read-only, from vmo_offset = one page: the mapping starts at page 2.
    ours::VirtAddr at = 0;
    ok &= expect("shell: vma_map(R, vmo_offset = page 2) = ",
                 ours::vma_map(vma, R, 0, vmo, PAGE, PAGE, &at), Status::Ok);
    if (at != 0) {
        auto const *const bytes = reinterpret_cast<u8 const volatile *>(at);
        u8 got[4] = {bytes[0], bytes[1], bytes[2], bytes[3]};
        ok &= same_bytes(second, got, sizeof(got));
    }

    // Read-write: a store through the mapping is what vmo_read finds.
    ours::VirtAddr rw = 0;
    ok &= expect("shell: vma_map(RW) = ", ours::vma_map(vma, RW, 0, vmo, 0, PAGE, &rw), Status::Ok);
    if (rw != 0) {
        *reinterpret_cast<u8 volatile *>(rw + 16) = 0x77;
        u8 back = 0;
        ok &=
            expect("shell: vmo_read(what the mapping wrote) = ", ours::vmo_read(vmo, &back, 16, 1),
                   Status::Ok);
        ok &= back == 0x77;
    }

    // A slice maps like any VMO, and no further than its own end.
    ours::Uandle slice = ours::Uandle::Invalid;
    ok &= expect("shell: vmo_create_child(Slice, page 2) = ",
                 ours::vmo_create_child(vmo, ours::VmoChildOptions::Slice, PAGE, PAGE, &slice),
                 Status::Ok);
    ours::VirtAddr sliced = 0;
    ok &=
        expect("shell: vma_map(R, the slice) = ", ours::vma_map(vma, R, 0, slice, 0, PAGE, &sliced),
               Status::Ok);
    if (sliced != 0) {
        ok &= *reinterpret_cast<u8 const volatile *>(sliced) == second[0];
    }
    ok &= expect("shell: vma_map(R, 2 pages of a 1-page slice) = ",
                 ours::vma_map(vma, R, 0, slice, 0, 2 * PAGE, &sliced), Status::OutOfRange);
    ok &= expect("shell: close(slice) = ", ours::handle_close(slice), Status::Ok);

    // The protection.
    ours::VirtAddr refused = 0;
    ok &= expect("shell: vma_map(W without R) = ",
                 ours::vma_map(vma, VmOptions::PermWrite, 0, vmo, 0, PAGE, &refused),
                 Status::InvArgs);
    ok &= expect("shell: vma_map(X without R) = ",
                 ours::vma_map(vma, VmOptions::PermExecute, 0, vmo, 0, PAGE, &refused),
                 Status::InvArgs);
    ok &= expect("shell: vma_map(RWX) = ",
                 ours::vma_map(vma, RW | VmOptions::PermExecute, 0, vmo, 0, PAGE, &refused),
                 Status::InvArgs);
    ok &= expect("shell: vma_map(R | CanMapWrite) = ",
                 ours::vma_map(vma, R | VmOptions::CanMapWrite, 0, vmo, 0, PAGE, &refused),
                 Status::InvArgs);

    // The rights.
    ok &= expect("shell: vma_map(RX, a VMO without Execute) = ",
                 ours::vma_map(vma, RX, 0, vmo, 0, PAGE, &refused), Status::AccessDenied);
    ours::Uandle unmappable = ours::Uandle::Invalid;
    ok &=
        expect("shell: handle_duplicate(vmo, without Map) = ",
               ours::handle_duplicate(vmo, Rights::Basic | Rights::Read, &unmappable), Status::Ok);
    ok &= expect("shell: vma_map(R, a handle without Map) = ",
                 ours::vma_map(vma, R, 0, unmappable, 0, PAGE, &refused), Status::AccessDenied);
    ok &=
        expect("shell: close(the copy without Map) = ", ours::handle_close(unmappable), Status::Ok);

    // The arguments.
    ok &= expect("shell: vma_map(length 0) = ", ours::vma_map(vma, R, 0, vmo, 0, 0, &refused),
                 Status::InvArgs);
    ok &= expect("shell: vma_map(unaligned vmo_offset) = ",
                 ours::vma_map(vma, R, 0, vmo, 8, PAGE, &refused), Status::InvArgs);
    ok &= expect("shell: vma_map(vma_offset without Specific) = ",
                 ours::vma_map(vma, R, PAGE, vmo, 0, PAGE, &refused), Status::InvArgs);
    ok &= expect("shell: vma_map(past the VMO) = ",
                 ours::vma_map(vma, R, 0, vmo, PAGE, 2 * PAGE, &refused), Status::OutOfRange);
    ok &= expect("shell: vma_map(Compact) = ",
                 ours::vma_map(vma, R | VmOptions::Compact, 0, vmo, 0, PAGE, &refused),
                 Status::NotSupported);

    // An address the kernel cannot write back: the mapping is taken down, so
    // the same fixed spot is free for the next call. Had it stayed, that call
    // would find the spot taken.
    constexpr auto SPOT = usize{1} << 30;
    ok &= expect("shell: vma_map(Specific, mapped_addr = kernel half) = ",
                 ours::vma_map(vma, R | VmOptions::Specific, SPOT, vmo, 0, PAGE,
                               reinterpret_cast<ours::VirtAddr *>(KERNEL)),
                 Status::InvArgs);
    ok &= expect("shell: vma_map(Specific, the same spot) = ",
                 ours::vma_map(vma, R | VmOptions::Specific, SPOT, vmo, 0, PAGE, &refused),
                 Status::Ok);

    // Handles of the wrong kind.
    ok &= expect("shell: vma_map(vma = a VMO) = ", ours::vma_map(vmo, R, 0, vmo, 0, PAGE, &refused),
                 Status::BadType);
    ok &= expect("shell: close(vmo) = ", ours::handle_close(vmo), Status::Ok);

    if (bootfs == ours::Uandle::Invalid || vmex == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vma_map of the BootFS skipped");
    } else {
        ok &= expect("shell: vma_map(RW, the BootFS) = ",
                     ours::vma_map(vma, RW, 0, bootfs, 0, PAGE, &refused), Status::AccessDenied);

        ours::Uandle copy = ours::Uandle::Invalid;
        ours::Uandle executable = ours::Uandle::Invalid;
        ok &= expect("shell: handle_duplicate(bootfs) = ",
                     ours::handle_duplicate(bootfs, Rights::SameRights, &copy), Status::Ok);
        ok &= expect("shell: vmo_replace_as_executable(the copy) = ",
                     ours::vmo_replace_as_executable(copy, vmex, &executable), Status::Ok);
        ours::VirtAddr code = 0;
        ok &= expect("shell: vma_map(RX, the executable BootFS) = ",
                     ours::vma_map(vma, RX, 0, executable, 0, PAGE, &code), Status::Ok);
        if (code != 0) {
            bootfs::Header header{};
            auto const *const bytes = reinterpret_cast<u8 const volatile *>(code);
            auto *const out = reinterpret_cast<u8 *>(&header);
            for (usize i = 0; i < sizeof(header); ++i) {
                out[i] = bytes[i];
            }
            ok &= header.magic == bootfs::Header::MAGIC;
        }
        ok &= expect("shell: close(the executable BootFS) = ", ours::handle_close(executable),
                     Status::Ok);
    }

    if (vmex == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vma_map of an executable snapshot skipped");
    } else {
        // A snapshot of an executable VMO: Write added, Execute taken away. It maps
        // read-write and not executable, and a store through that mapping takes a
        // copy of the page, which the executable VMO's own mapping never sees.
        ours::Uandle plain = ours::Uandle::Invalid;
        ours::Uandle executable = ours::Uandle::Invalid;
        ours::Uandle snapshot = ours::Uandle::Invalid;
        u8 const first[1] = {0x5a};
        ok &= expect("shell: vmo_create(to be executable) = ",
                     ours::vmo_create(PAGE, VmoOptions::None, &plain), Status::Ok);
        ok &= expect("shell: vmo_write(to be executable) = ",
                     ours::vmo_write(plain, first, 0, sizeof(first)), Status::Ok);
        ok &= expect("shell: vmo_replace_as_executable(it) = ",
                     ours::vmo_replace_as_executable(plain, vmex, &executable), Status::Ok);
        ours::VirtAddr code = 0;
        ok &= expect("shell: vma_map(RX, the executable VMO) = ",
                     ours::vma_map(vma, RX, 0, executable, 0, PAGE, &code), Status::Ok);
        ok &= expect("shell: vmo_create_child(the executable VMO, SnapshotFull) = ",
                     ours::vmo_create_child(executable, ours::VmoChildOptions::SnapshotFull, 0,
                                            PAGE, &snapshot),
                     Status::Ok);
        ok &= expect("shell: vma_map(RX, its snapshot) = ",
                     ours::vma_map(vma, RX, 0, snapshot, 0, PAGE, &refused), Status::AccessDenied);
        ours::VirtAddr data = 0;
        ok &= expect("shell: vma_map(RW, its snapshot) = ",
                     ours::vma_map(vma, RW, 0, snapshot, 0, PAGE, &data), Status::Ok);
        if (data != 0 && code != 0) {
            auto *const mine = reinterpret_cast<u8 volatile *>(data);
            auto const *const theirs = reinterpret_cast<u8 const volatile *>(code);
            ok &= mine[0] == first[0];
            mine[0] = 0xa5;
            ok &= mine[0] == 0xa5;
            ok &= theirs[0] == first[0];
            if (mine[0] != 0xa5 || theirs[0] != first[0]) {
                auto _ = ort::debug_line("shell: a store through the snapshot's mapping "
                                         "reached the executable VMO, or did not land");
            }
        }
        ok &= expect("shell: close(the snapshot) = ", ours::handle_close(snapshot), Status::Ok);
        ok &= expect("shell: close(the executable VMO) = ", ours::handle_close(executable),
                     Status::Ok);
    }

    if (ok) {
        auto _ = ort::debug_line("shell: vma_map checks passed");
    } else {
        auto _ = ort::debug_line("shell: vma_map checks FAILED");
    }
}

/// vma_allocate: a guard, a range of the root VMA that nothing can be
/// mapped into (K22), and a sub-region that can be (K23).
///
/// Runs in every image. What it pins down:
///   - the guard holds its range: a Specific mapping over it is refused, and
///     still is after the guard's handle is closed -- the range belongs to
///     the parent region, not to the handle;
///   - the guard itself allows nothing: mapping into it is AccessDenied, its
///     handle having none of Read, Write or Execute;
///   - a guard placed by the kernel holds its range the same way;
///   - a sub-region with CanMapRead and CanMapWrite, placed by the kernel:
///     a mapping at an offset inside it lands at that offset from its start,
///     and a store through it is what vmo_read finds; nothing goes past its
///     end, and closing its handle leaves its range reserved;
///   - a sub-region allows no more than it was given: one made with
///     CanMapRead alone refuses a writable mapping that the VMO and the
///     parent would both allow, and a nested sub-region asking for Execute
///     is AccessDenied;
///   - kernel placement reaches a region's last page: a nested sub-region
///     fills its parent exactly, and with page 0 taken an anywhere mapping
///     lands on page 1, after which the full region answers NotFound;
///   - CanMapSpecific: a sub-region without it refuses a place chosen in it,
///     for a mapping or a sub-region (AccessDenied), and takes one the
///     kernel places; a sub-region of it may have it all the same, and then
///     takes a chosen place; the root has it, so a Specific sub-region of
///     the root may ask for it too;
///   - the options: CanMap bits need the matching right on the parent
///     (AccessDenied), an offset without Specific is InvArgs, a Perm bit is
///     InvArgs, and an option named but not acted on yet (Compact) is
///     NotSupported;
///   - the arguments: a size of 0 and an unaligned offset are InvArgs;
///   - a value the kernel cannot write back releases the range: the same
///     spot can be reserved on the next call.
auto check_vma_allocate(ours::Uandle const vma) -> void {
    using ours::Status;
    using ours::VmoOptions;
    using ours::VmOptions;

    constexpr auto PAGE = u64{4096};
    // Past check_vma_map()'s fixed spot, and as far from anything else.
    constexpr auto SPOT = (usize{1} << 30) + 64 * PAGE;
    constexpr auto SPECIFIC_R = VmOptions::PermRead | VmOptions::Specific;

    if (vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vma_allocate checks FAILED (no VmaRoot)");
        return;
    }

    auto ok = true;
    ours::Uandle vmo = ours::Uandle::Invalid;
    if (!expect("shell: vmo_create = ", ours::vmo_create(PAGE, VmoOptions::None, &vmo),
                Status::Ok)) {
        auto _ = ort::debug_line("shell: vma_allocate checks FAILED");
        return;
    }

    ours::Uandle guard = ours::Uandle::Invalid;
    ours::VirtAddr at = 0;
    ok &= expect("shell: vma_allocate(Specific, 1 page) = ",
                 ours::vma_allocate(vma, VmOptions::Specific, SPOT, PAGE, &guard, &at), Status::Ok);
    // Where the root region starts, so that an address the kernel chose can be
    // turned back into an offset for a Specific request.
    auto const root_base = at - SPOT;
    ours::VirtAddr mapped = 0;
    ok &=
        expect("shell: vma_map(Specific, over the guard) = ",
               ours::vma_map(vma, SPECIFIC_R, SPOT, vmo, 0, PAGE, &mapped), Status::AlreadyExists);
    ok &= expect("shell: vma_map(into the guard) = ",
                 ours::vma_map(guard, VmOptions::PermRead, 0, vmo, 0, PAGE, &mapped),
                 Status::AccessDenied);
    ok &= expect("shell: close(guard) = ", ours::handle_close(guard), Status::Ok);
    ok &=
        expect("shell: vma_map(Specific, over the guard, handle closed) = ",
               ours::vma_map(vma, SPECIFIC_R, SPOT, vmo, 0, PAGE, &mapped), Status::AlreadyExists);

    // The same guard, placed by the kernel.
    ours::VirtAddr guard_at = 0;
    ok &= expect("shell: vma_allocate(no CanMap bit, anywhere, 1 page) = ",
                 ours::vma_allocate(vma, VmOptions::None, 0, PAGE, &guard, &guard_at), Status::Ok);
    ok &= expect("shell: vma_map(Specific, over the kernel-placed guard) = ",
                 ours::vma_map(vma, SPECIFIC_R, guard_at - root_base, vmo, 0, PAGE, &mapped),
                 Status::AlreadyExists);
    ok &= expect("shell: close(the kernel-placed guard) = ", ours::handle_close(guard), Status::Ok);

    // A sub-region to map into, placed by the kernel: what the launcher reserves
    // for a program's span before it maps each segment at its offset, which is
    // what CanMapSpecific is for.
    constexpr auto CAN_MAP_RW_SPECIFIC =
        VmOptions::CanMapRead | VmOptions::CanMapWrite | VmOptions::CanMapSpecific;
    ours::Uandle region = ours::Uandle::Invalid;
    ours::VirtAddr region_at = 0;
    ok &= expect(
        "shell: vma_allocate(CanMapRead | CanMapWrite | CanMapSpecific, anywhere, 2 pages) = ",
        ours::vma_allocate(vma, CAN_MAP_RW_SPECIFIC, 0, 2 * PAGE, &region, &region_at), Status::Ok);
    if (region_at == 0 || region_at % PAGE != 0) {
        auto _ = ort::debug_line("shell: the sub-region is not at a page boundary");
        ok = false;
    }
    ok &= expect("shell: vma_map(RW, Specific, page 1 of the sub-region) = ",
                 ours::vma_map(region,
                               VmOptions::PermRead | VmOptions::PermWrite | VmOptions::Specific,
                               PAGE, vmo, 0, PAGE, &mapped),
                 Status::Ok);
    if (mapped == region_at + PAGE) {
        // Read back through the VMO, not the same mapping: a mapping of some
        // other page would read back its own store just as well.
        *reinterpret_cast<u32 volatile *>(mapped) = 0x5ec7'0001;
        u32 back = 0;
        ok &= expect("shell: vmo_read(what the sub-region's mapping stored) = ",
                     ours::vmo_read(vmo, &back, 0, sizeof(back)), Status::Ok);
        if (back != 0x5ec7'0001) {
            auto _ = ort::debug_line("shell: a store through the sub-region's mapping "
                                     "did not reach the VMO");
            ok = false;
        }
    } else {
        auto _ =
            ort::debug_line("shell: page 1 of the sub-region was not mapped at its start + 1 page");
        ok = false;
    }
    ok &=
        expect("shell: vma_map(R, Specific, past the sub-region's end) = ",
               ours::vma_map(region, SPECIFIC_R, 2 * PAGE, vmo, 0, PAGE, &mapped), Status::InvArgs);
    ours::Uandle nested = ours::Uandle::Invalid;
    ours::VirtAddr nested_at = 0;
    ok &= expect("shell: vma_allocate(CanMapExecute, inside a sub-region without it) = ",
                 ours::vma_allocate(region, VmOptions::CanMapRead | VmOptions::CanMapExecute, 0,
                                    PAGE, &nested, &nested_at),
                 Status::AccessDenied);
    ok &= expect("shell: close(the sub-region) = ", ours::handle_close(region), Status::Ok);
    ok &= expect("shell: vma_map(Specific, over the sub-region, handle closed) = ",
                 ours::vma_map(vma, SPECIFIC_R, region_at - root_base, vmo, 0, PAGE, &mapped),
                 Status::AlreadyExists);

    // Only the sub-region can refuse here: the VMO's handle has Write, and so
    // does the root's. One made with CanMapRead alone gets a handle without
    // Write, and its mappings can be no more than readable.
    ours::Uandle read_only = ours::Uandle::Invalid;
    ours::VirtAddr read_only_at = 0;
    ok &= expect("shell: vma_allocate(CanMapRead, anywhere, 1 page) = ",
                 ours::vma_allocate(vma, VmOptions::CanMapRead, 0, PAGE, &read_only, &read_only_at),
                 Status::Ok);
    ok &= expect("shell: vma_map(RW, into a sub-region with CanMapRead only) = ",
                 ours::vma_map(read_only, VmOptions::PermRead | VmOptions::PermWrite, 0, vmo, 0,
                               PAGE, &mapped),
                 Status::AccessDenied);
    ok &= expect("shell: close(the read-only sub-region) = ", ours::handle_close(read_only),
                 Status::Ok);

    // The kernel places up to a region's last page, each case in a 2-page
    // sub-region of its own: a nested sub-region the whole size of its
    // parent; and, with page 0 taken, a mapping on page 1, the top, after
    // which the region is full and the next one is refused.
    ours::Uandle outer = ours::Uandle::Invalid;
    ours::VirtAddr outer_at = 0;
    ok &= expect("shell: vma_allocate(CanMapRead, anywhere, 2 pages) = ",
                 ours::vma_allocate(vma, VmOptions::CanMapRead, 0, 2 * PAGE, &outer, &outer_at),
                 Status::Ok);
    ok &= expect("shell: vma_allocate(CanMapRead, anywhere, the whole of its parent) = ",
                 ours::vma_allocate(outer, VmOptions::CanMapRead, 0, 2 * PAGE, &nested, &nested_at),
                 Status::Ok);
    ok &= nested_at == outer_at;
    ok &= expect("shell: close(the nested sub-region) = ", ours::handle_close(nested), Status::Ok);
    ok &= expect("shell: close(its parent) = ", ours::handle_close(outer), Status::Ok);

    ok &= expect("shell: vma_allocate(CanMapRead | CanMapSpecific, anywhere, 2 pages) = ",
                 ours::vma_allocate(vma, VmOptions::CanMapRead | VmOptions::CanMapSpecific, 0,
                                    2 * PAGE, &outer, &outer_at),
                 Status::Ok);
    ok &= expect("shell: vma_map(R, Specific, page 0 of the sub-region) = ",
                 ours::vma_map(outer, SPECIFIC_R, 0, vmo, 0, PAGE, &mapped), Status::Ok);
    ok &= expect("shell: vma_map(R, anywhere, the sub-region's last page) = ",
                 ours::vma_map(outer, VmOptions::PermRead, 0, vmo, 0, PAGE, &mapped), Status::Ok);
    ok &= mapped == outer_at + PAGE;
    ok &= expect("shell: vma_map(R, anywhere, the sub-region full) = ",
                 ours::vma_map(outer, VmOptions::PermRead, 0, vmo, 0, PAGE, &mapped),
                 Status::NotFound);
    ok &= expect("shell: close(the full sub-region) = ", ours::handle_close(outer), Status::Ok);

    // CanMapSpecific: without it a sub-region takes only what the kernel
    // places. A sub-region of it may have it all the same, and then takes a
    // place chosen in it: that sub-region itself landed where the kernel put
    // it, so the choice fixes only where things lie inside it.
    ours::Uandle plain = ours::Uandle::Invalid;
    ours::VirtAddr plain_at = 0;
    ok &= expect("shell: vma_allocate(CanMapRead, anywhere, 2 pages) = ",
                 ours::vma_allocate(vma, VmOptions::CanMapRead, 0, 2 * PAGE, &plain, &plain_at),
                 Status::Ok);
    ok &= expect("shell: vma_map(R, Specific, into a sub-region without CanMapSpecific) = ",
                 ours::vma_map(plain, SPECIFIC_R, 0, vmo, 0, PAGE, &mapped), Status::AccessDenied);
    // The flag is asked before the range, as in Zircon: a place past the end
    // of a region without it is AccessDenied, not InvArgs.
    ok &= expect("shell: vma_map(R, Specific, past the end of a sub-region without it) = ",
                 ours::vma_map(plain, SPECIFIC_R, 2 * PAGE, vmo, 0, PAGE, &mapped),
                 Status::AccessDenied);
    ours::Uandle inner = ours::Uandle::Invalid;
    ours::VirtAddr inner_at = 0;
    ok &= expect("shell: vma_allocate(Specific, into a sub-region without CanMapSpecific) = ",
                 ours::vma_allocate(plain, VmOptions::Specific, 0, PAGE, &inner, &inner_at),
                 Status::AccessDenied);
    ok &= expect("shell: vma_map(R, anywhere, into a sub-region without CanMapSpecific) = ",
                 ours::vma_map(plain, VmOptions::PermRead, 0, vmo, 0, PAGE, &mapped), Status::Ok);
    ok &= expect("shell: vma_allocate(CanMapRead | CanMapSpecific, anywhere, inside it) = ",
                 ours::vma_allocate(plain, VmOptions::CanMapRead | VmOptions::CanMapSpecific, 0,
                                    PAGE, &inner, &inner_at),
                 Status::Ok);
    ok &= expect("shell: vma_map(R, Specific, into its sub-region with CanMapSpecific) = ",
                 ours::vma_map(inner, SPECIFIC_R, 0, vmo, 0, PAGE, &mapped), Status::Ok);
    if (mapped != inner_at) {
        auto _ = ort::debug_line("shell: the Specific mapping is not at its offset");
        ok = false;
    }
    ok &= expect("shell: close(the inner sub-region) = ", ours::handle_close(inner), Status::Ok);
    ok &=
        expect("shell: close(the sub-region without CanMapSpecific) = ", ours::handle_close(plain),
               Status::Ok);

    // CanMap bits need the matching right on the parent.
    ours::Uandle no_execute = ours::Uandle::Invalid;
    // The root region's handle has Basic without Wait (RIGHTS_DEFAULT_VMA), so
    // the copy asks for Read and Write and nothing it does not hold.
    ok &= expect("shell: handle_duplicate(vma, Read | Write only) = ",
                 ours::handle_duplicate(vma, ours::Rights::Read | ours::Rights::Write, &no_execute),
                 Status::Ok);
    ok &= expect("shell: vma_allocate(CanMapExecute, parent handle without Execute) = ",
                 ours::vma_allocate(no_execute, VmOptions::CanMapRead | VmOptions::CanMapExecute, 0,
                                    PAGE, &nested, &nested_at),
                 Status::AccessDenied);
    ok &= expect("shell: close(the copy without Execute) = ", ours::handle_close(no_execute),
                 Status::Ok);

    // The options and the arguments.
    constexpr auto ELSEWHERE = SPOT + 16 * PAGE;
    constexpr auto FURTHER = SPOT + 32 * PAGE;
    constexpr auto CHOSEN = SPOT + 48 * PAGE;
    ok &= expect("shell: vma_allocate(Specific | CanMapRead) = ",
                 ours::vma_allocate(vma, VmOptions::Specific | VmOptions::CanMapRead, FURTHER, PAGE,
                                    &guard, &at),
                 Status::Ok);
    if (at != root_base + FURTHER) {
        auto _ = ort::debug_line("shell: the Specific sub-region is not at its offset");
        ok = false;
    }
    ok &= expect("shell: close(the Specific sub-region) = ", ours::handle_close(guard), Status::Ok);
    ok &= expect("shell: vma_allocate(an offset without Specific) = ",
                 ours::vma_allocate(vma, VmOptions::CanMapRead, PAGE, PAGE, &guard, &at),
                 Status::InvArgs);
    ok &= expect("shell: vma_allocate(Compact) = ",
                 ours::vma_allocate(vma, VmOptions::Compact, 0, PAGE, &guard, &at),
                 Status::NotSupported);
    ok &= expect("shell: vma_allocate(Specific | CanMapRead | CanMapSpecific) = ",
                 ours::vma_allocate(
                     vma, VmOptions::Specific | VmOptions::CanMapRead | VmOptions::CanMapSpecific,
                     CHOSEN, PAGE, &guard, &at),
                 Status::Ok);
    ok &= expect("shell: vma_map(R, Specific, into it) = ",
                 ours::vma_map(guard, SPECIFIC_R, 0, vmo, 0, PAGE, &mapped), Status::Ok);
    if (at != root_base + CHOSEN || mapped != at) {
        auto _ =
            ort::debug_line("shell: the chosen sub-region or its mapping is not at its offset");
        ok = false;
    }
    ok &= expect("shell: close(the chosen sub-region) = ", ours::handle_close(guard), Status::Ok);
    ok &=
        expect("shell: vma_allocate(Specific | PermRead) = ",
               ours::vma_allocate(vma, SPECIFIC_R, ELSEWHERE, PAGE, &guard, &at), Status::InvArgs);
    ok &= expect("shell: vma_allocate(size 0) = ",
                 ours::vma_allocate(vma, VmOptions::Specific, ELSEWHERE, 0, &guard, &at),
                 Status::InvArgs);
    ok &= expect("shell: vma_allocate(unaligned offset) = ",
                 ours::vma_allocate(vma, VmOptions::Specific, ELSEWHERE + 8, PAGE, &guard, &at),
                 Status::InvArgs);

    // A child_addr the kernel cannot write: the range goes back, so the same
    // spot can be taken right after.
    ok &= expect("shell: vma_allocate(child_addr = kernel half) = ",
                 ours::vma_allocate(vma, VmOptions::Specific, ELSEWHERE, PAGE, &guard,
                                    reinterpret_cast<ours::VirtAddr *>(KERNEL)),
                 Status::InvArgs);
    ok &= expect("shell: vma_allocate(the same spot) = ",
                 ours::vma_allocate(vma, VmOptions::Specific, ELSEWHERE, PAGE, &guard, &at),
                 Status::Ok);
    ok &= expect("shell: close(guard) = ", ours::handle_close(guard), Status::Ok);
    ok &= expect("shell: close(vmo) = ", ours::handle_close(vmo), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: vma_allocate checks passed");
    } else {
        auto _ = ort::debug_line("shell: vma_allocate checks FAILED");
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
auto check_process_create(ours::Uandle const job) -> void {
    using ours::Rights;
    using ours::Signals;
    using ours::Status;

    constexpr std::string_view NAME{"created"};

    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: process_create checks skipped (no root job)");
        return;
    }

    auto ok = true;
    ours::Uandle process = ours::Uandle::Invalid;
    ours::Uandle region = ours::Uandle::Invalid;

    ok &= expect("shell: process_create(Uandle::Invalid) = ",
                 ours::process_create(ours::Uandle::Invalid, NAME.data(), NAME.size(),
                                      ours::ProcessCreateOptions::None, &process, &region),
                 Status::BadHandle);
    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (expect("shell: channel_create = ",
               ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        ok &= expect("shell: process_create(job = a channel) = ",
                     ours::process_create(a, NAME.data(), NAME.size(),
                                          ours::ProcessCreateOptions::None, &process, &region),
                     Status::BadType);
        ok &= expect("shell: close(a) = ", ours::handle_close(a), Status::Ok);
        ok &= expect("shell: close(b) = ", ours::handle_close(b), Status::Ok);
    } else {
        ok = false;
    }
    ours::Uandle weak = ours::Uandle::Invalid;
    ok &= expect("shell: handle_duplicate(job, without ManageProcess) = ",
                 ours::handle_duplicate(job, Rights::Basic, &weak), Status::Ok);
    ok &= expect("shell: process_create(job without ManageProcess) = ",
                 ours::process_create(weak, NAME.data(), NAME.size(),
                                      ours::ProcessCreateOptions::None, &process, &region),
                 Status::AccessDenied);
    ok &= expect("shell: close(the weak job) = ", ours::handle_close(weak), Status::Ok);
    ok &=
        expect("shell: process_create(an option bit) = ",
               ours::process_create(job, NAME.data(), NAME.size(),
                                    static_cast<ours::ProcessCreateOptions>(1), &process, &region),
               Status::InvArgs);

    // Two handles, and the safety net.
    if (expect("shell: process_create = ",
               ours::process_create(job, NAME.data(), NAME.size(), ours::ProcessCreateOptions::None,
                                    &process, &region),
               Status::Ok)) {
        ok &= process != ours::Uandle::Invalid && region != ours::Uandle::Invalid &&
              process != region;
        ours::Uandle second = ours::Uandle::Invalid;
        ok &= expect("shell: handle_duplicate(process) = ",
                     ours::handle_duplicate(process, Rights::SameRights, &second), Status::Ok);
        ok &= expect("shell: close(region) = ", ours::handle_close(region), Status::Ok);
        ok &= expect("shell: close(the first process handle) = ", ours::handle_close(process),
                     Status::Ok);
        // One handle left: still Initial, so not terminated.
        auto observed = Signals::None;
        ok &= expect("shell: wait_one(process, TERMINATED, poll) with a handle left = ",
                     ours::object_wait_one(second, Signals::Terminated, 0, &observed),
                     Status::TimeOut);
        ok &= expect("shell: close(the last process handle) = ", ours::handle_close(second),
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
    static ours::Uandle filler[ROOM];
    u32 held = 0;
    while (held + 2 <= ROOM && ours::channel_create(ours::ChannelCreateOptions::None, &filler[held],
                                                    &filler[held + 1]) == Status::Ok) {
        held += 2;
    }
    // A pair needs two slots, so at most one is left; fill it too.
    if (held < ROOM &&
        ours::handle_duplicate(job, Rights::SameRights, &filler[held]) == Status::Ok) {
        held += 1;
    }
    ok &= held >= 2;
    if (held >= 2) {
        ok &= ours::handle_close(filler[held - 1]) == Status::Ok;
        ok &= ours::handle_close(filler[held - 2]) == Status::Ok;
        held -= 2;
        for (u32 i = 0; i < 3; ++i) {
            ok &= expect("shell: process_create(process_out = kernel half), table nearly full = ",
                         ours::process_create(job, NAME.data(), NAME.size(),
                                              ours::ProcessCreateOptions::None,
                                              reinterpret_cast<ours::Uandle *>(KERNEL), &region),
                         Status::InvArgs);
        }
        ok &= expect("shell: process_create after that = ",
                     ours::process_create(job, NAME.data(), NAME.size(),
                                          ours::ProcessCreateOptions::None, &process, &region),
                     Status::Ok);
        ok &= expect("shell: close(region) = ", ours::handle_close(region), Status::Ok);
        ok &= expect("shell: close(process) = ", ours::handle_close(process), Status::Ok);
    }
    for (u32 i = 0; i < held; ++i) {
        ok &= ours::handle_close(filler[i]) == Status::Ok;
    }

    if (ok) {
        auto _ = ort::debug_line("shell: process_create checks passed");
    } else {
        auto _ = ort::debug_line("shell: process_create checks FAILED");
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
auto check_thread_create(ours::Uandle const job) -> void {
    using ours::Rights;
    using ours::Status;

    constexpr std::string_view NAME{"never-started"};

    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: thread_create checks skipped (no root job)");
        return;
    }

    auto ok = true;
    ours::Uandle process = ours::Uandle::Invalid;
    ours::Uandle region = ours::Uandle::Invalid;
    if (!expect("shell: process_create = ",
                ours::process_create(job, "host", sizeof("host") - 1,
                                     ours::ProcessCreateOptions::None, &process, &region),
                Status::Ok)) {
        auto _ = ort::debug_line("shell: thread_create checks FAILED");
        return;
    }

    ours::Uandle thread = ours::Uandle::Invalid;
    ok &= expect("shell: thread_create(Uandle::Invalid) = ",
                 ours::thread_create(ours::Uandle::Invalid, NAME.data(), NAME.size(),
                                     ours::ThreadCreateOptions::None, &thread),
                 Status::BadHandle);
    ok &= expect("shell: thread_create(process = its root VMA) = ",
                 ours::thread_create(region, NAME.data(), NAME.size(),
                                     ours::ThreadCreateOptions::None, &thread),
                 Status::BadType);
    ours::Uandle weak = ours::Uandle::Invalid;
    ok &= expect("shell: handle_duplicate(process, without ManageThread) = ",
                 ours::handle_duplicate(process, Rights::Basic, &weak), Status::Ok);
    ok &= expect("shell: thread_create(process without ManageThread) = ",
                 ours::thread_create(weak, NAME.data(), NAME.size(),
                                     ours::ThreadCreateOptions::None, &thread),
                 Status::AccessDenied);
    ok &= expect("shell: close(the weak process handle) = ", ours::handle_close(weak), Status::Ok);
    ok &= expect("shell: thread_create(an option bit) = ",
                 ours::thread_create(process, NAME.data(), NAME.size(),
                                     static_cast<ours::ThreadCreateOptions>(1), &thread),
                 Status::InvArgs);
    ok &= expect("shell: close(region) = ", ours::handle_close(region), Status::Ok);

    // A thread made and closed, more than once.
    for (u32 i = 0; i < 4; ++i) {
        if (expect("shell: thread_create = ",
                   ours::thread_create(process, NAME.data(), NAME.size(),
                                       ours::ThreadCreateOptions::None, &thread),
                   Status::Ok)) {
            ok &= expect("shell: close(the unstarted thread) = ", ours::handle_close(thread),
                         Status::Ok);
        } else {
            ok = false;
        }
    }

    // The table full to exactly two slots, then failures that must leave none
    // behind: a thread needs one, so this is stricter than it needs to be, and
    // it is the same shape as process_create's, so the two read alike.
    constexpr u32 ROOM = ours::HANDLE_MAX_PER_PROCESS + 2;
    static ours::Uandle filler[ROOM];
    u32 held = 0;
    while (held + 2 <= ROOM && ours::channel_create(ours::ChannelCreateOptions::None, &filler[held],
                                                    &filler[held + 1]) == Status::Ok) {
        held += 2;
    }
    if (held < ROOM &&
        ours::handle_duplicate(job, Rights::SameRights, &filler[held]) == Status::Ok) {
        held += 1;
    }
    ok &= held >= 2;
    if (held >= 2) {
        ok &= ours::handle_close(filler[held - 1]) == Status::Ok;
        held -= 1;
        for (u32 i = 0; i < 3; ++i) {
            ok &= expect("shell: thread_create(out = kernel half), table nearly full = ",
                         ours::thread_create(process, NAME.data(), NAME.size(),
                                             ours::ThreadCreateOptions::None,
                                             reinterpret_cast<ours::Uandle *>(KERNEL)),
                         Status::InvArgs);
        }
        ok &= expect("shell: thread_create after that = ",
                     ours::thread_create(process, NAME.data(), NAME.size(),
                                         ours::ThreadCreateOptions::None, &thread),
                     Status::Ok);
    }
    for (u32 i = 0; i < held; ++i) {
        ok &= ours::handle_close(filler[i]) == Status::Ok;
    }

    // The process's last handle first, with a thread handle still open.
    ok &= expect("shell: close(the process) = ", ours::handle_close(process), Status::Ok);
    ok &= expect("shell: close(the thread, after its process) = ", ours::handle_close(thread),
                 Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: thread_create checks passed");
    } else {
        auto _ = ort::debug_line("shell: thread_create checks FAILED");
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
auto check_process_start(ours::Uandle const job) -> void {
    using ours::Rights;
    using ours::Signals;
    using ours::Status;

    constexpr auto ENTRY = ours::VirtAddr{0x400000};
    constexpr auto STACK = ours::VirtAddr{0x500000};

    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: process_start checks skipped (no root job)");
        return;
    }

    auto ok = true;
    ours::Uandle process = ours::Uandle::Invalid;
    ours::Uandle region = ours::Uandle::Invalid;
    ours::Uandle other = ours::Uandle::Invalid;
    ours::Uandle other_region = ours::Uandle::Invalid;
    ours::Uandle thread = ours::Uandle::Invalid;
    ours::Uandle other_thread = ours::Uandle::Invalid;
    if (!expect("shell: process_create = ",
                ours::process_create(job, "starts", sizeof("starts") - 1,
                                     ours::ProcessCreateOptions::None, &process, &region),
                Status::Ok) ||
        !expect("shell: process_create = ",
                ours::process_create(job, "other", sizeof("other") - 1,
                                     ours::ProcessCreateOptions::None, &other, &other_region),
                Status::Ok) ||
        !expect("shell: thread_create = ",
                ours::thread_create(process, "first", sizeof("first") - 1,
                                    ours::ThreadCreateOptions::None, &thread),
                Status::Ok) ||
        !expect("shell: thread_create = ",
                ours::thread_create(other, "theirs", sizeof("theirs") - 1,
                                    ours::ThreadCreateOptions::None, &other_thread),
                Status::Ok)) {
        auto _ = ort::debug_line("shell: process_start checks FAILED");
        return;
    }

    // The handles named.
    auto const H = ours::Uandle::Invalid;
    ok &= expect("shell: process_start(Uandle::Invalid) = ",
                 ours::process_start(H, thread, ENTRY, STACK, H, 0), Status::BadHandle);
    ok &= expect("shell: process_start(process = its root VMA) = ",
                 ours::process_start(region, thread, ENTRY, STACK, H, 0), Status::BadType);
    ok &= expect("shell: process_start(thread = a process) = ",
                 ours::process_start(process, process, ENTRY, STACK, H, 0), Status::BadType);
    ours::Uandle weak = ours::Uandle::Invalid;
    ok &= expect("shell: handle_duplicate(process, without Write) = ",
                 ours::handle_duplicate(process, Rights::Basic, &weak), Status::Ok);
    ok &= expect("shell: process_start(process without Write) = ",
                 ours::process_start(weak, thread, ENTRY, STACK, H, 0), Status::AccessDenied);
    ok &= expect("shell: close(the weak process handle) = ", ours::handle_close(weak), Status::Ok);
    ok &= expect("shell: process_start(another process's thread) = ",
                 ours::process_start(process, other_thread, ENTRY, STACK, H, 0),
                 Status::AccessDenied);

    // The handle moved.
    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (!expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        ok = false;
    } else {
        ours::Uandle stale = b;
        ok &= expect("shell: close(b) = ", ours::handle_close(b), Status::Ok);
        ok &=
            expect("shell: process_start(a handle that names nothing) = ",
                   ours::process_start(process, thread, ENTRY, STACK, stale, 0), Status::BadHandle);

        // A VMO's handle can be duplicated (a channel's cannot), so it is what
        // gives a copy without Transfer.
        ours::Uandle vmo = ours::Uandle::Invalid;
        ours::Uandle untransferable = ours::Uandle::Invalid;
        if (expect("shell: vmo_create = ", ours::vmo_create(4096, ours::VmoOptions::None, &vmo),
                   Status::Ok) &&
            expect("shell: handle_duplicate(vmo, without Transfer) = ",
                   ours::handle_duplicate(vmo, Rights::Duplicate | Rights::Read, &untransferable),
                   Status::Ok)) {
            ok &= expect("shell: process_start(a handle without Transfer) = ",
                         ours::process_start(process, thread, ENTRY, STACK, untransferable, 0),
                         Status::AccessDenied);
            ok &= expect("shell: close(that handle, gone all the same) = ",
                         ours::handle_close(untransferable), Status::BadHandle);
        } else {
            ok = false;
        }
        ok &= expect("shell: close(vmo) = ", ours::handle_close(vmo), Status::Ok);
        ok &= expect("shell: close(a) = ", ours::handle_close(a), Status::Ok);
    }

    // Consumed and closed on failure, twice: the process is as it was.
    for (u32 i = 0; i < 2; ++i) {
        ours::Uandle c = ours::Uandle::Invalid;
        ours::Uandle d = ours::Uandle::Invalid;
        if (!expect("shell: channel_create = ",
                    ours::channel_create(ours::ChannelCreateOptions::None, &c, &d), Status::Ok)) {
            ok = false;
            break;
        }
        ok &= expect("shell: process_start(no vDSO in the process) = ",
                     ours::process_start(process, thread, ENTRY, STACK, c, 0), Status::BadState);
        auto observed = Signals::None;
        ok &= expect("shell: wait_one(the peer, PEER_CLOSED, poll) = ",
                     ours::object_wait_one(d, Signals::PeerClosed, 0, &observed), Status::Ok);
        ok &= !!(observed & Signals::PeerClosed);
        ok &= expect("shell: close(c, gone) = ", ours::handle_close(c), Status::BadHandle);
        ok &= expect("shell: close(d) = ", ours::handle_close(d), Status::Ok);
    }

    // The thread's own handle as the handle argument.
    ok &= expect("shell: process_start(arg1 = the thread's own handle) = ",
                 ours::process_start(process, thread, ENTRY, STACK, thread, 0), Status::BadState);
    ok &=
        expect("shell: close(the thread, gone) = ", ours::handle_close(thread), Status::BadHandle);

    ok &= expect("shell: close(other thread) = ", ours::handle_close(other_thread), Status::Ok);
    ok &= expect("shell: close(region) = ", ours::handle_close(region), Status::Ok);
    ok &= expect("shell: close(other region) = ", ours::handle_close(other_region), Status::Ok);
    ok &= expect("shell: close(process) = ", ours::handle_close(process), Status::Ok);
    ok &= expect("shell: close(other process) = ", ours::handle_close(other), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: process_start checks passed");
    } else {
        auto _ = ort::debug_line("shell: process_start checks FAILED");
    }
}

/// Mapping the vDSO's VMO: an executable mapping of it is the registration of
/// a process's vDSO range, so it is allowed in one shape only (K23a), and that
/// shape is the vDSO's code segment, nothing more (system-abi-surface step 10).
///
/// Runs where userboot started this program, which is handed the VMO. On
/// processes made here and never started, so what is checked is what the
/// kernel says about the mapping, not what a running process does with it:
///   - the rights: the handle has no Write, so a writable mapping is
///     AccessDenied before the vDSO is looked at;
///   - the shape of an executable mapping, all InvArgs: not sealed (the
///     launcher asks for it, and the kernel refuses the vDSO's code without
///     it, so a launcher that forgot fails here and not silently); the vDSO's
///     data rather than its code; the whole image, code and data together, as
///     it used to be mapped; and the code alone, sealed, is Ok;
///   - the rest of it read-only: Ok, as often as wanted, and no registration;
///   - once per address space: a second executable mapping there is
///     AccessDenied, while another process's first is Ok -- the claim is the
///     address space's;
///   - by identity, not by content: a slice of the VMO, which has the same
///     pages, maps as an ordinary mapping and registers nothing, so a process
///     that has only that is still refused by process_start (BadState, no
///     vDSO), where one that mapped the VMO itself would not be.
auto check_vdso_map(ours::Uandle const job, ours::Uandle const vdso) -> void {
    using ours::Status;
    using ours::VmOptions;

    constexpr auto PAGE = u64{4096};
    constexpr auto RX = VmOptions::PermRead | VmOptions::PermExecute;
    constexpr auto SEALED_RX = RX | VmOptions::Sealed;
    constexpr auto SEALED_RW = VmOptions::PermRead | VmOptions::PermWrite | VmOptions::Sealed;

    if (job == ours::Uandle::Invalid || vdso == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vdso map checks skipped (no root job or vDSO)");
        return;
    }

    auto ok = true;
    ours::Uandle p = ours::Uandle::Invalid;
    ours::Uandle p_vma = ours::Uandle::Invalid;
    ours::Uandle q = ours::Uandle::Invalid;
    ours::Uandle q_vma = ours::Uandle::Invalid;
    ours::Uandle s = ours::Uandle::Invalid;
    ours::Uandle s_vma = ours::Uandle::Invalid;
    if (!expect("shell: process_create = ",
                ours::process_create(job, "vdso-p", sizeof("vdso-p") - 1,
                                     ours::ProcessCreateOptions::None, &p, &p_vma),
                Status::Ok) ||
        !expect("shell: process_create = ",
                ours::process_create(job, "vdso-q", sizeof("vdso-q") - 1,
                                     ours::ProcessCreateOptions::None, &q, &q_vma),
                Status::Ok) ||
        !expect("shell: process_create = ",
                ours::process_create(job, "vdso-s", sizeof("vdso-s") - 1,
                                     ours::ProcessCreateOptions::None, &s, &s_vma),
                Status::Ok)) {
        auto _ = ort::debug_line("shell: vdso map checks FAILED");
        return;
    }

    // Where its code is. The vDSO's headers are in the first page of its
    // image, which is the first page of its file too -- its first segment
    // starts at offset 0 -- so they can be read out of the VMO.
    u8 head[PAGE] = {};
    ok &=
        expect("shell: vmo_read(vdso, its headers) = ", ours::vmo_read(vdso, head, 0, sizeof(head)),
               Status::Ok);
    auto const program = launch::Program::read({head, sizeof(head)});
    u64 code = 0;
    u64 code_size = 0;
    u64 whole = 0;
    usize executable = 0;
    if (program.is_ok()) {
        for (auto const &segment: program.unwrap().segments()) {
            auto const first = segment.vaddr & ~(PAGE - 1);
            auto const end = (segment.vaddr + segment.mem_size + PAGE - 1) & ~(PAGE - 1);
            whole = std::max(whole, end);
            if ((segment.flags & launch::PF_X) != 0) {
                code = first;
                code_size = end - first;
                executable += 1;
            }
        }
    }
    if (!program.is_ok() || executable != 1 || code == 0) {
        auto _ = ort::debug_line("shell: vdso map checks FAILED (its code not found)");
        return;
    }
    char code_text[10];
    char pages_text[10];
    auto _ = ort::debug_line(
        {"shell: the vDSO is ", ort::decimal(static_cast<u32>(whole / PAGE), pages_text),
         " page(s), its code at page ", ort::decimal(static_cast<u32>(code / PAGE), code_text)});

    ours::VirtAddr at = 0;
    ok &= expect("shell: vma_map(vdso, RW, sealed) = ",
                 ours::vma_map(p_vma, SEALED_RW, 0, vdso, code, code_size, &at),
                 Status::AccessDenied);
    ok &= expect("shell: vma_map(vdso code, RX, not sealed) = ",
                 ours::vma_map(p_vma, RX, 0, vdso, code, code_size, &at), Status::InvArgs);
    ok &= expect("shell: vma_map(vdso data, RX, sealed) = ",
                 ours::vma_map(p_vma, SEALED_RX, 0, vdso, 0, PAGE, &at), Status::InvArgs);
    ok &= expect("shell: vma_map(the whole vdso, RX, sealed) = ",
                 ours::vma_map(p_vma, SEALED_RX, 0, vdso, 0, whole, &at), Status::InvArgs);
    ok &=
        expect("shell: vma_map(vdso data, read only, sealed) = ",
               ours::vma_map(p_vma, VmOptions::PermRead | VmOptions::Sealed, 0, vdso, 0, PAGE, &at),
               Status::Ok);
    ok &= expect("shell: vma_map(the whole vdso, read only) = ",
                 ours::vma_map(p_vma, VmOptions::PermRead, 0, vdso, 0, whole, &at), Status::Ok);
    ok &= expect("shell: vma_map(vdso code, RX, sealed) = ",
                 ours::vma_map(p_vma, SEALED_RX, 0, vdso, code, code_size, &at), Status::Ok);

    ours::VirtAddr again = 0;
    ok &= expect("shell: vma_map(vdso code) a second time = ",
                 ours::vma_map(p_vma, SEALED_RX, 0, vdso, code, code_size, &again),
                 Status::AccessDenied);
    ok &= expect("shell: vma_map(vdso code) into another process = ",
                 ours::vma_map(q_vma, SEALED_RX, 0, vdso, code, code_size, &again), Status::Ok);

    // The same pages by way of a slice: a mapping, and no registration.
    ours::Uandle slice = ours::Uandle::Invalid;
    ok &= expect("shell: vmo_create_child(vdso, Slice) = ",
                 ours::vmo_create_child(vdso, ours::VmoChildOptions::Slice, 0, whole, &slice),
                 Status::Ok);
    ok &= expect("shell: vma_map(the slice, RX) = ",
                 ours::vma_map(s_vma, RX, 0, slice, 0, whole, &again), Status::Ok);
    ok &= expect("shell: close(the slice) = ", ours::handle_close(slice), Status::Ok);

    // Sealing is not the vDSO's alone: any mapping may ask for it, and an
    // ordinary VMO's is accepted. What a sealed mapping then refuses is
    // checked in the kernel (vmaseal.test), since no program can unmap or
    // protect yet.
    ours::Uandle plain = ours::Uandle::Invalid;
    ok &= expect("shell: vmo_create = ", ours::vmo_create(PAGE, ours::VmoOptions::None, &plain),
                 Status::Ok);
    ok &= expect(
        "shell: vma_map(an ordinary VMO, sealed) = ",
        ours::vma_map(q_vma, VmOptions::PermRead | VmOptions::Sealed, 0, plain, 0, PAGE, &again),
        Status::Ok);
    ok &= expect("shell: close(the VMO) = ", ours::handle_close(plain), Status::Ok);

    ours::Uandle t = ours::Uandle::Invalid;
    ok &= expect(
        "shell: thread_create = ",
        ours::thread_create(s, "first", sizeof("first") - 1, ours::ThreadCreateOptions::None, &t),
        Status::Ok);
    ok &= expect("shell: process_start(a slice of the vDSO mapped, no vDSO) = ",
                 ours::process_start(s, t, 0x400000, 0x500000, ours::Uandle::Invalid, 0),
                 Status::BadState);
    ok &= expect("shell: close(the thread) = ", ours::handle_close(t), Status::Ok);

    for (auto const handle: {p_vma, q_vma, s_vma, p, q, s}) {
        ok &= ours::handle_close(handle) == Status::Ok;
    }

    if (ok) {
        auto _ = ort::debug_line("shell: vdso map checks passed");
    } else {
        auto _ = ort::debug_line("shell: vdso map checks FAILED");
    }
}

} // namespace

namespace shell::tests {
auto run(Inputs const &inputs) -> void {
    if (inputs.job != ours::Uandle::Invalid && inputs.peer == ours::Uandle::Invalid &&
        inputs.launch_program != nullptr) {
        run_probes(inputs.job, inputs.launch_program, inputs.launcher_context);
    }
    check_process_create(inputs.job);
    check_thread_create(inputs.job);
    check_process_start(inputs.job);
    check_vdso_map(inputs.job, inputs.vdso);
    check_handle_close(inputs.startup_to_close);
    check_channel_loopback();
    check_handle_transfer();
    check_port();
    check_wait_async();
    check_handle_quota();
    check_bad_pointers(ort::vdso_base());
    check_vmo_read();
    check_vmo_write(inputs.bootfs);
    check_vmo_create();
    check_bootfs(inputs.bootfs, ort::vdso_base());
    check_handle_duplicate(inputs.bootfs);
    check_vmo_create_child(inputs.bootfs);
    check_vmo_replace_as_executable(inputs.bootfs, inputs.vmex);
    check_vma_map(inputs.vma, inputs.bootfs, inputs.vmex);
    check_vma_allocate(inputs.vma);
    check_libc(static_cast<u8>(ort::vdso_base() >> 20));
}
} // namespace shell::tests
