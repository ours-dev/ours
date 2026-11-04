/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "shell_tests.hpp"

#include <ours/object_info.hpp>
#include <ours/resource.hpp>
#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <ours/version.hpp>
#include <ours/version_build.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <span>
#include <string_view>

#include <cstring>

#include "programs.hpp"
#include <bootfs/format.hpp>
#include <bootfs/view.hpp>
#include <elfload/diagnostics.hpp>
#include <fsio/fsio.hpp>
#include <launch/launch.hpp>
#include <launch/program.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/startup.hpp>
#include <ours_io/ours_io.hpp>
#include <ours_process/ours_process.hpp>
#include <ours_resource/client.hpp>
#include <ours_resource/ours_resource.hpp>
#include <ustl/option.hpp>
#include <wire/wire.hpp>

namespace {
using shell::spells;

auto report(std::string_view const what, ours::Status const status) -> void {
    auto _ = ort::debug_line({what, std::string_view{ours::to_string(status)}});
}

/// Two readings of the monotonic clock, with calls between them until it
/// moves or a bound runs out. The shell runs after the kernel has a clock
/// source, so the first reading is past the epoch, and the second is later:
/// a clock that answers 0, or the same value every time, is not wired up.
auto check_clock() -> void {
    auto const first = ours::clock_get_monotonic();
    auto second = first;
    for (auto i = 0; i < 1000; i = i + 1) {
        second = ours::clock_get_monotonic();
        if (second != first) {
            break;
        }
    }
    auto const hz = ours::ticks_per_second();
    auto const boot = ours::boot_offset();
    auto const version = ours::published_version();
    auto const kernel = ours::kernel_version();
    auto const kernel_ok =
        kernel != nullptr && std::string_view{kernel} == OURS_KERNEL_VERSION_STRING;
    if (first > 0 && second > first && hz > 0 && boot == 0 && version == 1 && kernel_ok) {
        auto _ = ort::debug_line("shell: clock checks passed");
        return;
    }
    char first_text[21];
    char second_text[21];
    char hz_text[21];
    char boot_text[21];
    char version_text[21];
    auto _ = ort::debug_line(
        {"shell: clock checks FAILED first=", ort::signed_decimal(first, first_text),
         " second=", ort::signed_decimal(second, second_text),
         " hz=", ort::signed_decimal(static_cast<i64>(hz), hz_text),
         " boot=", ort::signed_decimal(boot, boot_text),
         " version=", ort::signed_decimal(static_cast<i64>(version), version_text)});
}

/// Gives up a handle this program was started with, and checks that the
/// kernel then refuses it.
///
/// The first close has to succeed: the handle came from whoever started this
/// program, which sent it in the processargs message -- the job, the root
/// job itself when userboot started it and a duplicate of it when the root
/// did -- and it entered this process's table, under a value of this table's
/// choosing, when the runtime read that message before main. The second has
/// to fail with BadHandle, because the same number no longer names anything
/// here -- and that is the handle table's whole job, observed from the side
/// of the boundary that relies on it.
///
/// The shell keeps a duplicate of the job for the commands it runs, made
/// before this; the checks that start processes in the job come before it.
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
    /// The exception channels bound to it behaved. The process's: bound,
    /// refused a second, refused a write, delivered whole messages, and
    /// closed once the probe was dead. The faulting thread's, bound while its
    /// first exception was out: bound, refused a second, delivered nothing,
    /// and closed when the thread exited (check_thread_channel_closed()).
    bool exception_channel_ok;
    /// How many exceptions the channel delivered, and the type of the last.
    u32 exceptions_seen;
    ours::ExceptionType exception_type;
    /// Every job debugger channel run_probe() was given heard the probe's
    /// start, as answer_process_starting() checks; true when it was given
    /// none.
    bool debuggers_ok;
};

/// Asks |exception| for the faulting thread and its process, as a handler
/// does to reach the state it would read or fix, and checks what comes back:
/// the process answers object_get_info(Process), the thread is a live handle
/// that does not. The koids are not compared with ExceptionInfo's yet:
/// object_get_info(HandleBasic) now answers them, and the comparison is
/// slice 2, step 1, of .agents/tasks/exception-channels/.
auto check_exception_tasks(ours::Uandle const exception) -> bool {
    using ours::Status;

    auto ok = true;
    auto thread = ours::Uandle::Invalid;
    auto process = ours::Uandle::Invalid;
    if (auto const got = ours::exception_get_process(exception, &process); got != Status::Ok) {
        report("shell: exception_get_process = ", got);
        ok = false;
    } else {
        ours::InfoProcess info{};
        if (auto const asked = ours::object_get_info(process, ours::InfoTopic::Process, &info,
                                                     sizeof(info), nullptr, nullptr);
            asked != Status::Ok) {
            report("shell: object_get_info(the exception's process) = ", asked);
            ok = false;
        }
    }
    if (auto const got = ours::exception_get_thread(exception, &thread); got != Status::Ok) {
        report("shell: exception_get_thread = ", got);
        ok = false;
    } else {
        ours::InfoProcess info{};
        if (auto const asked = ours::object_get_info(thread, ours::InfoTopic::Process, &info,
                                                     sizeof(info), nullptr, nullptr);
            asked != Status::BadType) {
            report("shell: object_get_info(the exception's thread, Process) = ", asked);
            ok = false;
        }
    }
    // Both were charged to this process; they go at once.
    auto _ = ours::handle_close(thread);
    auto _ = ours::handle_close(process);
    return ok;
}

/// The faulting thread of a probe, and the exception channel bound to it.
/// Both are Uandle::Invalid until the probe's first exception; the caller of
/// pass_on_exceptions() closes them.
struct ThreadChannel {
    ours::Uandle thread;
    ours::Uandle channel;
};

/// Asks |exception| for the thread it stopped, and binds an exception channel
/// to that thread while it is stopped: a supervisor learns of a thread from
/// its first exception and from then on hears from that thread before
/// anyone bound to its process. A second channel is refused. The thread and
/// the channel go to |out|; a value a refused call wrote is not kept, since
/// it names nothing. Slice 2, step 2a, of
/// .agents/tasks/exception-channels/PLAN.md.
auto bind_thread_channel(ours::Uandle const exception, ThreadChannel *const out) -> bool {
    using ours::Status;

    if (auto const got = ours::exception_get_thread(exception, &out->thread); got != Status::Ok) {
        report("shell: exception_get_thread, to bind its channel = ", got);
        out->thread = ours::Uandle::Invalid;
        return false;
    }
    if (auto const bound = ours::task_create_exception_channel(
            out->thread, ours::ExceptionChannelOptions::None, &out->channel);
        bound != Status::Ok) {
        report("shell: a thread's exception channel = ", bound);
        out->channel = ours::Uandle::Invalid;
        return false;
    }

    auto second = ours::Uandle::Invalid;
    if (auto const again = ours::task_create_exception_channel(
            out->thread, ours::ExceptionChannelOptions::None, &second);
        again != Status::AlreadyBound) {
        report("shell: a second exception channel on a thread = ", again);
        if (again == Status::Ok) {
            auto _ = ours::handle_close(second);
        }
        return false;
    }
    return true;
}

/// Once the probe is dead: its thread's channel delivered nothing -- the
/// thread was past its own slot when the channel was bound, and faulted only
/// once -- and closed when the thread exited. The dead thread's slot then
/// refuses a new channel with BadState: it shut at the thread's exit, not
/// when the thread object goes, which |thread|'s handle still holds off. A
/// slot that stayed open would take a channel nothing could ever close.
auto check_thread_channel_closed(ThreadChannel const &thread) -> bool {
    using ours::Signals;
    using ours::Status;

    // A poll, not a wait: the thread's slot shuts before the thread leaves
    // its process (ThreadDispatcher::on_exit()), and the process's channel,
    // closed by now, closes only once its last thread has left. So the
    // reader sees PeerClosed already, and a slot that did not shut is
    // reported here instead of hanging the shell.
    constexpr ours::Instant NOW = 0;
    auto ok = true;
    auto seen = Signals::None;
    if (auto const polled = ours::object_wait_one(thread.channel, Signals::PeerClosed, NOW, &seen);
        polled != Status::Ok) {
        report("shell: a dead thread's exception channel is still open: ", polled);
        ok = false;
    } else if ((seen & Signals::Readable) != Signals::None) {
        auto _ = ort::debug_line({"shell: a dead thread's exception channel delivered something"});
        ok = false;
    }

    // Its TERMINATED, raised as it left its process, which it did before the
    // process's channel closed; RUNNING went down with it. A poll for the
    // same reason as above.
    seen = Signals::None;
    if (auto const polled = ours::object_wait_one(thread.thread, Signals::Terminated, NOW, &seen);
        polled != Status::Ok) {
        report("shell: a dead thread does not read Terminated: ", polled);
        ok = false;
    } else if ((seen & (Signals::ThreadRunning | Signals::ThreadSuspended)) != Signals::None) {
        auto _ = ort::debug_line({"shell: a dead thread still reads Running or Suspended"});
        ok = false;
    }

    auto again = ours::Uandle::Invalid;
    if (auto const bound = ours::task_create_exception_channel(
            thread.thread, ours::ExceptionChannelOptions::None, &again);
        bound != Status::BadState) {
        report("shell: an exception channel on a dead thread = ", bound);
        if (bound == Status::Ok) {
            auto _ = ours::handle_close(again);
        }
        ok = false;
    }
    return ok;
}

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
/// With |handle|, each exception is marked Handled before its handle is
/// closed, and the probe resumes instead of dying. False, with the reason
/// reported, when the channel misbehaved. Steps 5 and 6 of
/// .agents/tasks/exception-channels/PLAN.md.
///
/// The first exception also binds a channel to the faulting thread, into
/// |thread| (bind_thread_channel()).
auto pass_on_exceptions(ours::Uandle const channel, bool const handle, ProbeRun *const run,
                        ThreadChannel *const thread) -> bool {
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
        auto ok = check_exception_tasks(exception);
        if (thread->channel == ours::Uandle::Invalid) {
            ok &= bind_thread_channel(exception, thread);
        }

        // Handled resumes the thread where it stopped. Without it the close
        // means TryNext: no other handler is bound, so the kernel kills the
        // probe, and the channel closes with it.
        if (handle) {
            constexpr auto HANDLED = ours::ExceptionState::Handled;
            if (auto const set = ours::object_set_property(
                    exception, ours::Property::ExceptionState, &HANDLED, sizeof(HANDLED));
                set != Status::Ok) {
                report("shell: marking an exception handled = ", set);
                ok = false;
            }
        }
        auto _ = ours::handle_close(exception);
        if (!ok) {
            return false;
        }
    }
}

/// The koid |handle| names, or 0 when it names nothing.
auto koid_of(ours::Uandle const handle) -> ours::KoId {
    ours::InfoHandleBasic basic{};
    if (ours::object_get_info(handle, ours::InfoTopic::HandleBasic, &basic, sizeof(basic), nullptr,
                              nullptr) != ours::Status::Ok) {
        return 0;
    }
    return basic.koid;
}

/// Reads the next exception off |debugger| into |info| and |exception|,
/// waiting for one, and answers false, reported, when the channel closed or
/// the read failed.
auto read_exception(ours::Uandle const debugger, ours::ExceptionInfo *const info,
                    ours::Uandle *const exception) -> bool {
    using ours::Signals;
    using ours::Status;

    auto seen = Signals::None;
    if (auto const waited = ours::object_wait_one(debugger, Signals::Readable | Signals::PeerClosed,
                                                  ours::INSTANT_INFINITE, &seen);
        waited != Status::Ok || !(seen & Signals::Readable)) {
        report("shell: waiting on a debugger channel = ", waited);
        return false;
    }
    u32 bytes = 0;
    u32 handles = 0;
    auto const read = ours::channel_read(debugger, ours::ChannelReadOptions::None, info, exception,
                                         sizeof(*info), 1, &bytes, &handles);
    if (read != Status::Ok || bytes != sizeof(*info) || handles != 1) {
        report("shell: reading a debugger channel = ", read);
        auto _ = ours::handle_close(*exception);
        return false;
    }
    return true;
}

/// Reads, from each of |debuggers| in the order given, the ProcessStarting a
/// job's debugger channel hears when |process| starts, and answers it by
/// closing the exception: the process's first thread waits in the kernel,
/// before its first instruction, until every debugger has. Checks what a
/// debugger is promised: the exception names the process and its first
/// thread; exception_get_process and exception_get_thread hand out those
/// two; the thread has no debugger channel of its own (InvArgs); and the
/// next debugger hears nothing until this one has answered. A start of
/// another process, which a debugger on the job hears too, is answered and
/// skipped. False, with the reason reported, when anything differed.
auto answer_process_starting(std::span<ours::Uandle const> const debuggers,
                             ours::Uandle const process) -> bool {
    using ours::Signals;
    using ours::Status;

    if (debuggers.empty()) {
        return true;
    }
    auto const koid = koid_of(process);
    if (koid == 0) {
        auto _ = ort::debug_line("shell: the started probe has no koid");
        return false;
    }

    auto ok = true;
    auto const check = [&ok](bool const holds, std::string_view const what) {
        if (!holds) {
            auto _ = ort::debug_line({"shell: ProcessStarting: ", what});
            ok = false;
        }
    };
    for (usize i = 0; i < debuggers.size(); ++i) {
        // Bounded: nothing else starts on dbg_1 while this runs, so more than
        // a few starts of other processes means the probe's never came.
        constexpr u32 MAX_OTHERS = 8;
        ours::ExceptionInfo info{};
        auto exception = ours::Uandle::Invalid;
        u32 others = 0;
        for (;;) {
            if (!read_exception(debuggers[i], &info, &exception)) {
                return false;
            }
            if (info.pid == koid) {
                break;
            }
            auto _ = ours::handle_close(exception);
            if (++others == MAX_OTHERS) {
                auto _ = ort::debug_line("shell: ProcessStarting: the probe's start never came");
                return false;
            }
        }
        check(info.type == ours::ExceptionType::ProcessStarting, "not ProcessStarting");

        if (i + 1 < debuggers.size()) {
            constexpr ours::Instant NOW = 0;
            check(ours::object_wait_one(debuggers[i + 1], Signals::Readable, NOW, nullptr) ==
                      Status::TimeOut,
                  "the next debugger heard the start before this one answered");
        }

        auto task = ours::Uandle::Invalid;
        ok &= expect("shell: exception_get_process(ProcessStarting) = ",
                     ours::exception_get_process(exception, &task), Status::Ok);
        check(koid_of(task) == koid, "exception_get_process names another process");
        auto _ = ours::handle_close(task);
        ok &= expect("shell: exception_get_thread(ProcessStarting) = ",
                     ours::exception_get_thread(exception, &task), Status::Ok);
        check(info.tid != 0 && koid_of(task) == info.tid,
              "exception_get_thread names another thread");
        // Started, though held before its first instruction: RUNNING, and
        // nothing else of a thread's.
        {
            constexpr ours::Instant NOW = 0;
            auto seen = Signals::None;
            check(ours::object_wait_one(task, Signals::ThreadRunning, NOW, &seen) == Status::Ok &&
                      (seen & (Signals::Terminated | Signals::ThreadSuspended)) == Signals::None,
                  "the starting thread does not read Running alone");
        }
        auto unbound = ours::Uandle::Invalid;
        ok &= expect("shell: task_create_exception_channel(a thread, Debugger) = ",
                     ours::task_create_exception_channel(
                         task, ours::ExceptionChannelOptions::Debugger, &unbound),
                     Status::InvArgs);
        auto _ = ours::handle_close(task);

        // TryNext, the default: the start goes on whatever the debugger says.
        auto _ = ours::handle_close(exception);
    }
    return ok;
}

/// Starts fault_probe under |job| with |probe| as argv[1] with |launcher|,
/// and waits for it to answer or die. The handle to the process the launcher
/// returns is handed back in run.process.
///
/// The probe is started the way every program on the runtime is: a
/// processargs message on its bootstrap channel (argv = {"bin/fault_probe",
/// probe}), written once the process exists -- its root VMA, which the
/// launcher hands back, and a duplicate of its process handle are two of the
/// three startup handles -- with the far end of a second channel, as User0,
/// for the answer. The answer
/// cannot come back on the bootstrap channel, because the runtime closes that
/// as soon as it has read the message: waiting on it, this would see
/// PeerClosed before the probe had even reached main and count every probe
/// as killed.
///
/// With |debuggers|, debugger channels bound to |job|, each is read for the
/// probe's start before anything else is done to it, since the probe does
/// not run until each has answered (answer_process_starting()).
auto run_probe(ours::Uandle const job, std::string_view const probe, bool const handle,
               shell::LaunchProgram const launcher, void *const launcher_context,
               std::span<ours::Uandle const> const debuggers = {}) -> ProbeRun {
    using ours::Signals;
    using ours::Status;

    ProbeRun run{};

    // The message first, while this holds no handles: a name that does not
    // encode leaves nothing to clean up.
    u8 message[128];
    u32 length = 0;
    u32 const infos[] = {ours::handle_info(ours::HandleType::User0, 0),
                         ours::handle_info(ours::HandleType::VmaRoot, 0),
                         ours::handle_info(ours::HandleType::ProcSelf, 0)};
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
    run.debuggers_ok = answer_process_starting(debuggers, process);

    // An exception channel on the probe before it can fault: it waits in its
    // runtime for the processargs written below.
    auto exceptions = ours::Uandle::Invalid;
    run.exception_channel_ok = bind_exception_channel(process, &exceptions);

    // Its handle to itself, as every starter hands one over: a duplicate with
    // every right, since this keeps |process| to watch the probe. One that
    // cannot be made fails the write below, which says so.
    auto self = ours::Uandle::Invalid;
    if (auto const duplicated = ours::handle_duplicate(process, ours::Rights::SameRights, &self);
        duplicated != Status::Ok) {
        report("shell: duplicating the probe's process handle = ", duplicated);
    }

    // Then the message. Its handles leave this process with it, whatever the
    // write answers; the probe waits for it, and if it never comes, sees this
    // end close and ends itself.
    ours::Uandle handed[] = {answer_there, root_vma, self};
    status = ours::channel_write(bootstrap, ours::ChannelWriteOptions::None, message, length,
                                 handed, static_cast<u32>(std::size(handed)));
    // This end has nothing more to say, and holding it would keep a handle
    // charged to this process for nothing.
    auto _ = ours::handle_close(bootstrap);
    if (status != Status::Ok) {
        report("shell: writing the probe's processargs = ", status);
    }

    // The exceptions first: a probe that faults waits in the kernel for its
    // answer, and would never get to the answer channel below.
    ThreadChannel thread{.thread = ours::Uandle::Invalid, .channel = ours::Uandle::Invalid};
    if (exceptions != ours::Uandle::Invalid) {
        run.exception_channel_ok &= pass_on_exceptions(exceptions, handle, &run, &thread);
        auto _ = ours::handle_close(exceptions);
    }
    // Only a probe that faulted has one.
    if (thread.channel != ours::Uandle::Invalid) {
        run.exception_channel_ok &= check_thread_channel_closed(thread);
    }
    auto _ = ours::handle_close(thread.channel);
    auto _ = ours::handle_close(thread.thread);

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

    // A dead process is no longer its job's: neither listed nor found by
    // koid, though this handle keeps it from being destroyed -- the one case
    // where only the removal at death (ProcessDispatcher::
    // finish_dead_transition()), not the one at destruction, takes it off.
    ours::InfoHandleBasic basic{};
    ok &= expect("shell: object_get_info(the control, HandleBasic) = ",
                 ours::object_get_info(process, InfoTopic::HandleBasic, &basic, sizeof(basic),
                                       nullptr, nullptr),
                 Status::Ok);
    static ours::KoId KOIDS[256];
    usize listed = 0;
    ok &= expect(
        "shell: object_get_info(job, JobProcesses, the control ended) = ",
        ours::object_get_info(job, InfoTopic::JobProcesses, KOIDS, sizeof(KOIDS), &listed, nullptr),
        Status::Ok);
    for (usize i = 0; i < listed; ++i) {
        if (KOIDS[i] == basic.koid) {
            ok = false;
            auto _ = ort::debug_line("shell: the job still lists the control after it ended");
        }
    }
    auto child = ours::Uandle::Invalid;
    ok &= expect("shell: object_get_child(job, the ended control) = ",
                 ours::object_get_child(job, basic.koid, ours::Rights::SameRights, &child),
                 Status::NotFound);

    // 4, a topic number nothing names.
    ok &= expect("shell: object_get_info(an unknown topic) = ",
                 ours::object_get_info(process, static_cast<InfoTopic>(4), &info, sizeof(info),
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
    using ours::ExceptionType;
    /// Each probe, the exception it raises, and whether this handles it. Only
    /// a system call from outside the vDSO can be resumed without changing
    /// the thread's registers: the kernel refuses the call with BadSyscall
    /// and the probe goes on, answers "survived" and returns 1. A fault
    /// resumed unchanged would only fault again.
    struct Probe {
        std::string_view name;
        ExceptionType type;
        bool handled;
    };
    constexpr Probe PROBES[] = {
        {"pf_unmapped", ExceptionType::FatalPageFault, false},
        {"pf_kernel", ExceptionType::FatalPageFault, false},
        {"pf_write_vdso", ExceptionType::FatalPageFault, false},
        {"pf_exec_vdso_data", ExceptionType::FatalPageFault, false},
        {"pf_write_text", ExceptionType::FatalPageFault, false},
        {"pf_exec_data", ExceptionType::FatalPageFault, false},
        {"pf_write_relro", ExceptionType::FatalPageFault, false},
        {"ud2", ExceptionType::UndefinedInstruction, false},
        {"gp_hlt", ExceptionType::General, false},
        {"gp_int80", ExceptionType::General, false},
        // General, not SwBreakpoint: the #BP gate is DPL 0, so an int3 from
        // ring 3 raises #GP instead.
        {"gp_int3", ExceptionType::General, false},
        // A divide error has no kind of its own (task/exception.hpp).
        {"de", ExceptionType::General, false},
        {"db_tf", ExceptionType::HwBreakpoint, false},
        {"gp_io", ExceptionType::General, false},
        {"trap_outside_vdso", ExceptionType::PolicyError, true},
        {"pf_write_rodata", ExceptionType::FatalPageFault, false},
    };

    // What a channel is refused for, once: a job's own channel, which this
    // kernel does not bind yet (only its debugger channels,
    // check_job_debugger()), and a property set on an object that is not an
    // exception (the root job's handle carries SetProperty).
    auto exception_checks_ok = true;
    auto unbound = ours::Uandle::Invalid;
    exception_checks_ok &= expect(
        "shell: task_create_exception_channel(job) = ",
        ours::task_create_exception_channel(job, ours::ExceptionChannelOptions::None, &unbound),
        ours::Status::NotSupported);
    auto not_a_task = ours::Uandle::Invalid;
    exception_checks_ok &=
        expect("shell: exception_get_thread(job) = ", ours::exception_get_thread(job, &not_a_task),
               ours::Status::BadType);
    constexpr auto HANDLED = ours::ExceptionState::Handled;
    exception_checks_ok &= expect(
        "shell: object_set_property(job, ExceptionState) = ",
        ours::object_set_property(job, ours::Property::ExceptionState, &HANDLED, sizeof(HANDLED)),
        ours::Status::BadType);

    u32 killed = 0;
    u32 survived = 0;
    u32 as_expected = 0;
    for (auto const &probe: PROBES) {
        auto const run = run_probe(job, probe.name, probe.handled, launcher, launcher_context);
        if (!run.started) {
            // The control, and with it the launcher's verdict below, is never
            // reached; say so, or the launcher would go unchecked unnoticed.
            auto _ = ort::debug_line(
                "shell: launcher checks FAILED (a probe before its control did not start)");
            return;
        }
        auto const code = return_code_of(run.process);
        auto _ = ours::handle_close(run.process);

        // Each probe faults once, so its channel has to have delivered exactly
        // one exception, of the kind the probe raises.
        auto const delivered = run.exception_channel_ok && run.exceptions_seen == 1 &&
                               run.exception_type == probe.type;
        char type_text[18];
        auto const type = ort::hex(static_cast<u64>(run.exception_type), type_text);

        char code_text[21];
        if (code.is_ok() && code.unwrap() == ours::TASK_RETCODE_EXCEPTION_KILL) {
            auto _ = ort::debug_line({"shell: probe ", probe.name, " killed (retcode ",
                                      ort::signed_decimal(code.unwrap(), code_text),
                                      ") after exception ", type});
            ++killed;
            if (delivered && !probe.handled) {
                ++as_expected;
            }
        } else {
            auto const ended =
                code.is_ok() ? ort::signed_decimal(code.unwrap(), code_text) : "unknown";
            auto const answer = std::string_view(run.answer, run.answer_size);
            if (probe.handled) {
                auto _ = ort::debug_line({"shell: probe ", probe.name, " handled exception ", type,
                                          " and survived (retcode ", ended, ", answered \"", answer,
                                          "\")"});
            } else {
                auto _ = ort::debug_line({"shell: probe ", probe.name, " SURVIVED (retcode ", ended,
                                          ", answered \"", answer, "\")"});
            }
            ++survived;
            // Returning 1 from main, after answering "survived": the
            // probe's own exit (fault_probe.cpp, ExitCode::SURVIVED).
            if (delivered && probe.handled && code.is_ok() && code.unwrap() == 1 &&
                spells(run.answer, run.answer_size, "survived")) {
                ++as_expected;
            }
        }
    }

    char killed_text[10];
    char survived_text[10];
    auto _ = ort::debug_line({"shell: probes done (", ort::decimal(killed, killed_text),
                              " killed, ", ort::decimal(survived, survived_text), " survived)"});

    constexpr std::string_view CONTROL{"none"};
    auto _ = ort::debug_line("shell: starting the control probe with the launcher");
    auto const control = run_probe(job, CONTROL, false, launcher, launcher_context);
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
    auto const channels_all = as_expected == std::size(PROBES);
    auto const control_quiet =
        control.started && control.exception_channel_ok && control.exceptions_seen == 0;
    auto _ =
        ort::debug_line({"shell: exception channels: ", ort::decimal(as_expected, channels_text),
                         " probes delivered their exception and ended as expected",
                         control_quiet ? ", none for the control" : ", and the control?"});
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

/// A job's debugger channels (task_create_exception_channel with Debugger),
/// on |job|, with probes started by |launcher|:
///   - the refusals: an unknown option bit is InvArgs, and a process's
///     debugger channel NotSupported; a thread's is InvArgs, checked on the
///     probe's first thread (answer_process_starting()), and the job's own
///     channel stays NotSupported (run_probes());
///   - two bound to the job both hear a probe's start, in the order they were
///     bound and one at a time, and the probe then runs as any other: it
///     answers "unknown", its process channel hears no exception;
///   - a debugger whose reader has gone holds no start up: with the first
///     closed, a second probe starts with only the second told;
///   - MAX_JOB_DEBUGGER_CHANNELS bound at once, one more is AlreadyBound, and
///     once they are closed a new one binds again: a closed one stops
///     counting.
/// Where run_probes() runs (dbg_1): it needs fault_probe and the launcher.
auto check_job_debugger(ours::Uandle const job, shell::LaunchProgram const launcher,
                        void *const launcher_context) -> void {
    using ours::ExceptionChannelOptions;
    using ours::Status;

    auto ok = true;
    auto unbound = ours::Uandle::Invalid;
    ok &= expect(
        "shell: task_create_exception_channel(job, an unknown option) = ",
        ours::task_create_exception_channel(job, static_cast<ExceptionChannelOptions>(2), &unbound),
        Status::InvArgs);
    auto process = ours::Uandle::Invalid;
    auto region = ours::Uandle::Invalid;
    ok &= expect("shell: process_create(for a debugger channel) = ",
                 ours::process_create(job, "debugged", sizeof("debugged") - 1,
                                      ours::ProcessCreateOptions::None, &process, &region),
                 Status::Ok);
    ok &= expect(
        "shell: task_create_exception_channel(a process, Debugger) = ",
        ours::task_create_exception_channel(process, ExceptionChannelOptions::Debugger, &unbound),
        Status::NotSupported);
    auto _ = ours::handle_close(region);
    auto _ = ours::handle_close(process);

    ours::Uandle debuggers[2] = {ours::Uandle::Invalid, ours::Uandle::Invalid};
    for (auto &debugger: debuggers) {
        ok &= expect(
            "shell: task_create_exception_channel(job, Debugger) = ",
            ours::task_create_exception_channel(job, ExceptionChannelOptions::Debugger, &debugger),
            Status::Ok);
    }
    auto const told = [&ok](ProbeRun const &run, std::string_view const which) {
        auto const held = run.started && run.debuggers_ok && !run.killed &&
                          spells(run.answer, run.answer_size, "unknown") &&
                          run.exception_channel_ok && run.exceptions_seen == 0;
        if (!held) {
            auto _ = ort::debug_line({"shell: job debugger: ", which,
                                      " probe was not told, or did not run as any other"});
            ok = false;
        }
        auto _ = ours::handle_close(run.process);
    };
    if (ok) {
        told(run_probe(job, "none", false, launcher, launcher_context, debuggers), "the first");
        auto _ = ours::handle_close(debuggers[0]);
        told(run_probe(job, "none", false, launcher, launcher_context,
                       std::span{debuggers}.subspan(1)),
             "with one reader gone, the second");
    }
    auto _ = ours::handle_close(debuggers[1]);

    // Nothing starts while these are bound: no one reads them, and a start
    // would wait for each.
    static ours::Uandle BOUND[ours::MAX_JOB_DEBUGGER_CHANNELS];
    u32 count = 0;
    for (auto &debugger: BOUND) {
        if (ours::task_create_exception_channel(job, ExceptionChannelOptions::Debugger,
                                                &debugger) == Status::Ok) {
            ++count;
        }
    }
    if (count != ours::MAX_JOB_DEBUGGER_CHANNELS) {
        auto _ = ort::debug_line("shell: job debugger: fewer than the most a job takes bound");
        ok = false;
    }
    auto extra = ours::Uandle::Invalid;
    ok &=
        expect("shell: task_create_exception_channel(job, Debugger, one too many) = ",
               ours::task_create_exception_channel(job, ExceptionChannelOptions::Debugger, &extra),
               Status::AlreadyBound);
    for (auto const debugger: std::span{BOUND}.first(count)) {
        auto _ = ours::handle_close(debugger);
    }
    ok &=
        expect("shell: task_create_exception_channel(job, Debugger, after closing them) = ",
               ours::task_create_exception_channel(job, ExceptionChannelOptions::Debugger, &extra),
               Status::Ok);
    auto _ = ours::handle_close(extra);

    if (ok) {
        auto _ = ort::debug_line("shell: job debugger checks passed");
    } else {
        auto _ = ort::debug_line("shell: job debugger checks FAILED");
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
               ours::port_create(static_cast<ours::PortCreateOptions>(2), &port), Status::InvArgs);
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
///   - a port closed with waits still armed tears them down and is
///     destroyed at once, though each wait holds a reference to it; the
///     object they watched firing later finds nothing. The kernel's
///     "object: port (koid N) destroyed" belongs between the two marker lines
///     below; the boot log is the check.
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

    // Three waits armed on an idle channel, then their port closed under them.
    Uandle late = Uandle::Invalid;
    if (expect("shell: wait_async port_create(late) = ",
               ours::port_create(ours::PortCreateOptions::None, &late), Status::Ok) &&
        expect("shell: wait_async channel_create(c, d) again = ",
               ours::channel_create(ours::ChannelCreateOptions::None, &c, &d), Status::Ok)) {
        for (u64 key = 14; key < 17; ++key) {
            ok &= expect(
                "shell: wait_async(d, Readable, then the port goes) = ",
                ours::object_wait_async(d, late, key, Signals::Readable, WaitAsyncOptions::None),
                Status::Ok);
        }
        auto _ = ort::debug_line("shell: closing a port with 3 waits armed");
        ok &= expect("shell: wait_async close(late) = ", ours::handle_close(late), Status::Ok);
        auto _ = ort::debug_line("shell: the port with 3 waits armed is closed");
        ok &= expect("shell: wait_async write(c) after the port went = ",
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

/// PORT_MAX_OBSERVERS, which counts a port's live observers: armed, or
/// matched with their packet still queued.
///   - Armed on an idle channel end until NoResource; closing that end cancels
///     them all, and a wait can be armed again.
///   - Armed on a readable end until NoResource, each queued at once: queued
///     packets count too. Closing the end changes nothing, since the packets
///     stay; reading one packet makes room for one wait.
auto check_port_observer_limit() -> void {
    using ours::PortPacket;
    using ours::Signals;
    using ours::Status;
    using ours::Uandle;
    using ours::WaitAsyncOptions;

    auto ok = true;
    auto const arm_until_refused = [](Uandle const object, Uandle const port, u32 *const armed) {
        *armed = 0;
        for (;;) {
            auto const status = ours::object_wait_async(object, port, *armed, Signals::Readable,
                                                        WaitAsyncOptions::None);
            if (status != Status::Ok || *armed > ours::PORT_MAX_OBSERVERS) {
                return status;
            }
            *armed += 1;
        }
    };

    // Armed, then cancelled.
    Uandle port = Uandle::Invalid;
    Uandle a = Uandle::Invalid;
    Uandle b = Uandle::Invalid;
    if (!expect("shell: limit port_create = ",
                ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok) ||
        !expect("shell: limit channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        auto _ = ort::debug_line("shell: port observer limit checks FAILED");
        return;
    }
    u32 armed = 0;
    ok &= expect("shell: wait_async(b idle) until refused = ", arm_until_refused(b, port, &armed),
                 Status::NoResource);
    ok &= armed == ours::PORT_MAX_OBSERVERS;
    ok &= expect("shell: limit close(b) = ", ours::handle_close(b), Status::Ok);
    ok &= expect("shell: wait_async(a) after b's waits were cancelled = ",
                 ours::object_wait_async(a, port, 0, Signals::Readable, WaitAsyncOptions::None),
                 Status::Ok);
    ok &= ours::handle_close(a) == Status::Ok && ours::handle_close(port) == Status::Ok;

    // Queued at once.
    if (!expect("shell: limit port_create again = ",
                ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok) ||
        !expect("shell: limit channel_create again = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        auto _ = ort::debug_line("shell: port observer limit checks FAILED");
        return;
    }
    constexpr std::string_view PING{"ping"};
    ok &= expect("shell: limit write(a) = ",
                 ours::channel_write(a, ours::ChannelWriteOptions::None, PING.data(),
                                     static_cast<u32>(PING.size()), nullptr, 0),
                 Status::Ok);
    ok &=
        expect("shell: wait_async(b readable) until refused = ", arm_until_refused(b, port, &armed),
               Status::NoResource);
    ok &= armed == ours::PORT_MAX_OBSERVERS;
    ok &= expect("shell: limit close(b) with its packets queued = ", ours::handle_close(b),
                 Status::Ok);
    ok &= expect("shell: wait_async(a) with the queue full of packets = ",
                 ours::object_wait_async(a, port, 0, Signals::Readable, WaitAsyncOptions::None),
                 Status::NoResource);
    PortPacket got;
    ok &= expect("shell: limit port_wait = ", ours::port_wait(port, 0, &got), Status::Ok);
    ok &= expect("shell: wait_async(a) after one packet was read = ",
                 ours::object_wait_async(a, port, 0, Signals::Readable, WaitAsyncOptions::None),
                 Status::Ok);
    ok &= ours::handle_close(a) == Status::Ok && ours::handle_close(port) == Status::Ok;

    if (ok) {
        auto _ = ort::debug_line("shell: port observer limit checks passed");
    } else {
        auto _ = ort::debug_line("shell: port observer limit checks FAILED");
    }
}

/// Where this shell's namespace has the root's Launcher on dev: its /svc,
/// where every protocol use lives (design D3).
constexpr std::string_view LAUNCHER_PATH{"/svc/ours.process.Launcher"};
static_assert(LAUNCHER_PATH.ends_with(ours_process::LAUNCHER_NAME));

/// routing_checks in this shell's namespace on dev: /boot/ and its install
/// name (programs.hpp), so that no program name is spelled here. The program
/// check_spawn() starts, and the file whose VMO the Launcher checks hand in.
constexpr std::string_view BOOT_PREFIX{"/boot/"};
constexpr auto ROUTING_CHECKS_PATH = [] {
    std::array<char, BOOT_PREFIX.size() + user_programs::ROUTING_CHECKS.size()> path{};
    std::ranges::copy(BOOT_PREFIX, path.begin());
    std::ranges::copy(user_programs::ROUTING_CHECKS, path.begin() + BOOT_PREFIX.size());
    return path;
}();

auto routing_checks_path() -> std::string_view {
    return {ROUTING_CHECKS_PATH.data(), ROUTING_CHECKS_PATH.size()};
}

/// bin/profile in this shell's namespace on dev, the program check_profile()
/// starts.
constexpr auto PROFILE_PATH = [] {
    std::array<char, BOOT_PREFIX.size() + user_programs::PROFILE.size()> path{};
    std::ranges::copy(BOOT_PREFIX, path.begin());
    std::ranges::copy(user_programs::PROFILE, path.begin() + BOOT_PREFIX.size());
    return path;
}();

/// The sampling profiler driven from user space: bin/profile, started through
/// the root's Launcher with this shell's namespace, takes the Sampling
/// resource and the root job from /svc, describes every process from outside
/// it, and a copy of itself it starts as a worker, creates a session for both
/// targets, samples a workload of system calls and user arithmetic, prints
/// the dump for prof-fold with markup for every process its samples name, and
/// exits 0 when it recorded kernel and user samples, some in its own code and
/// some in its worker's. Only a component has the namespace it needs: on
/// dbg_1, skipped.
auto check_profile(bool const component, ours::Uandle const job) -> void {
    if (!component || job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: profile checks skipped (no namespace)");
        return;
    }
    auto const path = std::string_view{PROFILE_PATH.data(), PROFILE_PATH.size()};
    std::string_view const argv[] = {path};
    auto const spawned = fsio::spawn(job, path, argv);
    if (spawned.is_err()) {
        report("shell: starting profile = ", spawned.unwrap_err());
        auto _ = ort::debug_line("shell: profile checks FAILED");
        return;
    }
    auto const code = return_code_of(spawned.unwrap());
    auto _ = ours::handle_close(spawned.unwrap());
    if (code.is_ok() && code.unwrap() == 0) {
        auto _ = ort::debug_line("shell: profile checks passed");
    } else {
        auto _ = ort::debug_line("shell: profile checks FAILED");
    }
}

/// One handle more than the Add* calls may give: a processargs message
/// carries CHANNEL_MAX_MSG_HANDLES, 64, and the Launcher adds three of its
/// own, the process's root region, its vDSO and its handle to itself
/// (launch::LAUNCHER_HANDLE_INFO).
constexpr auto TOO_MANY_HANDLES = usize{ours::CHANNEL_MAX_MSG_HANDLES} - 3 + 1;

/// The status of |result|: Ok, or its error.
template <typename T>
auto status_of(ustl::Result<T, ours::Status> const &result) -> ours::Status {
    if (result.is_ok()) {
        return ours::Status::Ok;
    }
    return result.unwrap_err();
}

/// One message read off a channel, owning the handles it carried until
/// take() claims one: what a server answered, or why it closed.
class Received final {
  public:
    Received() = default;
    Received(Received const &) = delete;
    auto operator=(Received const &) -> Received & = delete;

    ~Received() {
        for (auto index = u32{0}; index < m_num_handles; ++index) {
            if (m_handles[index] != ours::Uandle::Invalid) {
                // This process's own; a close that fails has no one to tell.
                auto _ = ours::handle_close(m_handles[index]);
            }
        }
    }

    /// Waits for |channel| to have a message or to close, and reads the
    /// message: PeerClosed when it closed with nothing on it, which is how a
    /// connection closed without an epitaph reads.
    [[nodiscard]]
    auto receive(ours::Uandle const channel) -> ours::Status {
        auto observed = ours::Signals::None;
        if (auto const status =
                ours::object_wait_one(channel, ours::Signals::Readable | ours::Signals::PeerClosed,
                                      ours::INSTANT_INFINITE, &observed);
            status != ours::Status::Ok) {
            return status;
        }
        if (!(observed & ours::Signals::Readable)) {
            return ours::Status::PeerClosed;
        }
        return ours::channel_read(channel, ours::ChannelReadOptions::None, m_bytes.data(),
                                  m_handles.data(), static_cast<u32>(m_bytes.size()),
                                  static_cast<u32>(m_handles.size()), &m_num_bytes, &m_num_handles);
    }

    [[nodiscard]]
    auto message() const -> wire::Message {
        return {
            .bytes = std::span{m_bytes}.first(m_num_bytes),
            .handles = std::span{m_handles}.first(m_num_handles),
        };
    }

    /// The status of the epitaph this message is; none for any other.
    [[nodiscard]]
    auto epitaph() const -> ustl::Option<ours::Status> {
        auto const header = wire::decode_header(message().bytes, wire::Direction::ToClient);
        if (header.is_err() || !wire::is_epitaph(header.unwrap())) {
            return ustl::NONE;
        }
        auto const status = wire::decode_epitaph(message());
        if (status.is_err()) {
            return ustl::some(ours::Status::InvArgs);
        }
        return ustl::some(status.unwrap());
    }

    /// Takes |handle|, which the message carried: the caller closes it.
    auto take(ours::Uandle const handle) -> void {
        for (auto index = u32{0}; index < m_num_handles; ++index) {
            if (m_handles[index] == handle) {
                m_handles[index] = ours::Uandle::Invalid;
            }
        }
    }

  private:
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_bytes{};
    std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> m_handles{};
    u32 m_num_bytes = 0;
    u32 m_num_handles = 0;
};

/// Writes |message| on |channel|; its handles go whatever the answer.
auto write_message(ours::Uandle const channel, wire::Message const message) -> ours::Status {
    return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                               static_cast<u32>(message.bytes.size()), message.handles.data(),
                               static_cast<u32>(message.handles.size()));
}

/// A new connection to the root's ours.process.Launcher, through /svc.
/// Open is one-way, so a route that failed shows only later, as an epitaph
/// on the connection.
auto connect_launcher() -> ustl::Result<ours::Uandle, ours::Status> {
    auto client = ours::Uandle::Invalid;
    auto server = ours::Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
        status != ours::Status::Ok) {
        return ustl::err(status);
    }
    if (auto const status = fsio::connect(LAUNCHER_PATH, server); status != ours::Status::Ok) {
        auto _ = ours::handle_close(client);
        return ustl::err(status);
    }
    return ustl::ok(client);
}

/// A VMO of routing_checks' file without Execute: File.GetBackingMemory with
/// READ alone, on a connection opened without EXECUTE.
auto readable_vmo() -> ustl::Result<ours::Uandle, ours::Status> {
    auto const file = fsio::open(routing_checks_path(), ours_io::Flags::PermReadBytes);
    if (file.is_err()) {
        return ustl::err(file.unwrap_err());
    }
    auto const vmo = fsio::get_backing_memory(file.unwrap(), ours_io::VmoFlags::Read);
    auto _ = ours::handle_close(file.unwrap());
    return vmo;
}

/// Sends Launch on |launcher| for |executable| (consumed) under a duplicate
/// of |job|, and returns what it answered: the reply's status, an epitaph's,
/// or PeerClosed. These checks expect no process; one that comes anyway is
/// closed, and answers Ok, which no check here expects.
auto launch_answer(ours::Uandle const launcher, ours::Uandle const executable,
                   ours::Uandle const job) -> ours::Status {
    using ours::Status;

    auto launch_job = ours::Uandle::Invalid;
    if (auto const status = ours::handle_duplicate(job, ours::Rights::SameRights, &launch_job);
        status != Status::Ok) {
        auto _ = ours::handle_close(executable);
        return status;
    }
    // The one two-way request on each connection these checks make.
    constexpr auto TXID = u32{1};
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<ours::Uandle, 2> handles{};
    auto const request = ours_process::encode_launch(
        bytes, handles,
        {.txid = TXID, .info = {.executable = executable, .job = launch_job, .name = "refused"}});
    if (request.is_err()) {
        auto _ = ours::handle_close(executable);
        auto _ = ours::handle_close(launch_job);
        return request.unwrap_err();
    }
    // A write that finds the connection closed is not the answer: the
    // epitaph it was closed with, if any, is still there to read.
    if (auto const status = write_message(launcher, request.unwrap());
        status != Status::Ok && status != Status::PeerClosed) {
        return status;
    }
    Received reply;
    if (auto const status = reply.receive(launcher); status != Status::Ok) {
        return status;
    }
    if (auto const epitaph = reply.epitaph(); epitaph.is_some()) {
        return *epitaph;
    }
    auto const decoded = ours_process::decode_launch_reply(reply.message(), TXID);
    if (decoded.is_err()) {
        return Status::InvArgs;
    }
    // The process, if one was started after all, closes with |reply|.
    return decoded->status;
}

/// AddHandles on |launcher| with |count| duplicates of |job|, the first
/// typed |first_type| and the rest User0, each with its index as the
/// argument. One-way: the answer is the write's.
auto add_handles(ours::Uandle const launcher, ours::Uandle const job, usize const count,
                 ours::HandleType const first_type) -> ours::Status {
    std::array<ours_process::HandleInfo, wire::MAX_MESSAGE_HANDLES> infos{};
    auto const close_made = [&infos](usize const made) {
        for (auto index = usize{0}; index < made; ++index) {
            auto _ = ours::handle_close(infos[index].handle);
        }
    };
    for (auto made = usize{0}; made < count; ++made) {
        auto handle = ours::Uandle::Invalid;
        if (auto const status = ours::handle_duplicate(job, ours::Rights::SameRights, &handle);
            status != ours::Status::Ok) {
            close_made(made);
            return status;
        }
        auto type = ours::HandleType::User0;
        if (made == 0) {
            type = first_type;
        }
        infos[made] = {.handle = handle, .id = ours::handle_info(type, static_cast<u16>(made))};
    }
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> handles{};
    auto const request =
        ours_process::encode_add_handles(bytes, handles, std::span{infos}.first(count));
    if (request.is_err()) {
        close_made(count);
        return request.unwrap_err();
    }
    return write_message(launcher, request.unwrap());
}

/// ours.process.Launcher in the root (component-framework design,
/// "Events: the shell running a program"), from a client's side: only where
/// the root started the shell as a component (dev); dbg_1 has no root and
/// skips this. Each Launch on a connection of its own, since a Launch, and
/// the closing of a connection, resets what its Add* calls added:
///   - the shell holds no VMEX: the root alone does (design I6);
///   - fsio::spawn() of a program /boot does not have is NotFound, before
///     anything reaches the Launcher;
///   - a VMO without Execute is AccessDenied: launch::start() maps it with
///     Execute before it reads a byte of it (D14);
///   - a reserved handle type (VmaRoot, the new process's own region, which
///     only the Launcher hands out) answers the next Launch with InvArgs;
///   - 63 handles, one more than a processargs leaves room for beside the
///     Launcher's two, answer the next Launch with InvArgs;
///   - a malformed message closes the connection with no epitaph.
/// Both InvArgs answer before the VMO, which lacks Execute, is looked at: the
/// mark the AddHandles set in the connection's state answers first. The
/// program that does start, and exits 0, is check_spawn()'s.
auto check_launcher_service(bool const component, ours::Uandle const job, ours::Uandle const vmex)
    -> void {
    using ours::Status;

    if (!component) {
        auto _ = ort::debug_line("shell: launcher service checks skipped (no namespace)");
        return;
    }

    auto ok = true;
    if (vmex != ours::Uandle::Invalid) {
        ok = false;
        auto _ = ort::debug_line("shell: holds vmex beside the root's Launcher");
    }
    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: launcher service checks FAILED (no job)");
        return;
    }

    // No executable: spawn() finds nothing to open, and launches nothing. A
    // process it started after all is closed rather than kept.
    constexpr std::string_view MISSING{"/boot/bin/no_such_program"};
    std::string_view const missing_argv[] = {MISSING};
    auto const missing = fsio::spawn(job, MISSING, missing_argv);
    if (missing.is_ok()) {
        auto _ = ours::handle_close(missing.unwrap());
    }
    ok &= expect("shell: spawn(a program /boot does not have) = ", status_of(missing),
                 Status::NotFound);

    // One fresh connection per case, and the answer of its one Launch, with
    // what |prepare| writes first.
    auto const answer = [&ok, job](std::string_view const what, auto const &prepare) -> Status {
        auto const launcher = connect_launcher();
        if (!expect("shell: connect(/svc/ours.process.Launcher) = ", status_of(launcher),
                    Status::Ok)) {
            ok = false;
            return Status::Ok;
        }
        auto status = prepare(launcher.unwrap());
        if (status == Status::Ok) {
            auto const vmo = readable_vmo();
            status = status_of(vmo);
            if (vmo.is_ok()) {
                status = launch_answer(launcher.unwrap(), vmo.unwrap(), job);
            }
        } else {
            auto _ = ort::debug_line({what, std::string_view{ours::to_string(status)}});
        }
        auto _ = ours::handle_close(launcher.unwrap());
        return status;
    };
    auto const nothing = [](ours::Uandle) { return Status::Ok; };
    auto const reserved = [job](ours::Uandle const launcher) {
        return add_handles(launcher, job, 1, ours::HandleType::VmaRoot);
    };
    auto const too_many = [job](ours::Uandle const launcher) {
        return add_handles(launcher, job, TOO_MANY_HANDLES, ours::HandleType::User0);
    };
    ok &= expect("shell: Launch(a VMO without Execute) = ",
                 answer("shell: Launch(a VMO without Execute), preparing = ", nothing),
                 Status::AccessDenied);
    ok &= expect("shell: Launch(after AddHandles with a VmaRoot) = ",
                 answer("shell: AddHandles(a VmaRoot) = ", reserved), Status::InvArgs);
    ok &= expect("shell: Launch(after AddHandles with 63 handles) = ",
                 answer("shell: AddHandles(63 handles) = ", too_many), Status::InvArgs);

    // A malformed message, eight bytes where a header takes sixteen: the
    // connection is closed with no epitaph, so there is nothing to read.
    if (auto const launcher = connect_launcher(); launcher.is_ok()) {
        std::array<u8, 8> garbage{};
        ok &=
            expect("shell: write(a malformed message to the Launcher) = ",
                   write_message(launcher.unwrap(), {.bytes = garbage, .handles = {}}), Status::Ok);
        Received closed;
        ok &= expect("shell: the Launcher's answer to a malformed message = ",
                     closed.receive(launcher.unwrap()), Status::PeerClosed);
        auto _ = ours::handle_close(launcher.unwrap());
    } else {
        ok &= expect("shell: connect(/svc/ours.process.Launcher) = ", launcher.unwrap_err(),
                     Status::Ok);
    }

    if (ok) {
        auto _ = ort::debug_line("shell: launcher service checks passed");
    } else {
        auto _ = ort::debug_line("shell: launcher service checks FAILED");
    }
}

/// fsio::spawn() end to end, the way a typed command runs on dev (design
/// D12): routing_checks with the argument `child`, which binds the namespace
/// it was handed, a clone of this shell's, reads one file of /boot through
/// it, and exits 0. So the boot covers the whole path a command takes: Open
/// and GetBackingMemory with EXECUTE, the Add* calls, AddNames' clones, and
/// the Launch. Only on dev.
auto check_spawn(bool const component, ours::Uandle const job) -> void {
    using ours::Status;

    if (!component) {
        auto _ = ort::debug_line("shell: spawn checks skipped (no namespace)");
        return;
    }
    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: spawn checks FAILED (no job)");
        return;
    }

    auto ok = true;
    std::string_view const argv[] = {routing_checks_path(), "child"};
    auto const spawned = fsio::spawn(job, argv[0], argv);
    ok &= expect("shell: spawn(routing_checks child) = ", status_of(spawned), Status::Ok);
    if (spawned.is_ok()) {
        // TERMINATED first, since until then a kill may still change the
        // code, then the code itself.
        auto const process = spawned.unwrap();
        ours::InfoProcess info{};
        if (expect("shell: wait_one(routing_checks child, Terminated) = ",
                   ours::object_wait_one(process, ours::Signals::Terminated, ours::INSTANT_INFINITE,
                                         nullptr),
                   Status::Ok) &&
            expect("shell: object_get_info(routing_checks child) = ",
                   ours::object_get_info(process, ours::InfoTopic::Process, &info, sizeof(info),
                                         nullptr, nullptr),
                   Status::Ok)) {
            char code[21];
            auto _ = ort::debug_line({"shell: routing_checks child exited with ",
                                      ort::signed_decimal(info.return_code, code)});
            ok &= info.return_code == 0;
        } else {
            ok = false;
        }
        auto _ = ours::handle_close(process);
    }

    if (ok) {
        auto _ = ort::debug_line("shell: spawn checks passed");
    } else {
        auto _ = ort::debug_line("shell: spawn checks FAILED");
    }
}

/// The composed tree at /vfs (macro-fs design): fshost's directory, in which
/// `boot` is the root's built-in BootFS directory mounted as a remote entry.
/// Listing /vfs names the mount point; opening /vfs/boot binds to the
/// root's /boot server through fshost's forwarded Open; reading behind it
/// walks two servers. Only on dev, where the root offers /vfs.
auto check_vfs(bool const component) -> void {
    using ours::Status;
    using ours::Uandle;

    if (!component) {
        auto _ = ort::debug_line("shell: vfs checks skipped (no namespace)");
        return;
    }

    auto ok = true;

    // The mount point stands in its parent's listing as a directory.
    auto const vfs =
        fsio::open("/vfs", ours_io::Flags::ProtocolDirectory | ours_io::Flags::PermEnumerate);
    ok &= expect("shell: open(/vfs) = ", status_of(vfs), Status::Ok);
    if (vfs.is_ok()) {
        std::array<u8, ours_io::MAX_BUF> buffer{};
        auto found = false;
        for (;;) {
            auto const read = fsio::read_dirents(vfs.unwrap(), buffer);
            if (!expect("shell: read_dirents(/vfs) = ", status_of(read), Status::Ok) ||
                read.unwrap() == 0) {
                break;
            }
            auto reader = ours_io::DirentReader(std::span{buffer}.first(read.unwrap()));
            for (auto next = reader.next(); next.is_ok() && next->is_some(); next = reader.next()) {
                auto const &dirent = next.unwrap().value();
                found = found ||
                        (dirent.name == "boot" && dirent.type == ours_io::DirentType::Directory);
            }
        }
        if (!found) {
            ok = false;
            auto _ = ort::debug_line("shell: no boot entry in /vfs FAILED");
        }
        ok &= expect("shell: close(/vfs) = ", fsio::close(vfs.unwrap()), Status::Ok);
    } else {
        ok = false;
    }

    // Through the mount point: this program's own file, two servers away.
    auto const file = fsio::open("/vfs/boot/bin/shell", ours_io::Flags::PermReadBytes);
    ok &= expect("shell: open(/vfs/boot/bin/shell) = ", status_of(file), Status::Ok);
    if (file.is_ok()) {
        std::array<u8, 4> magic{};
        auto const read = fsio::read(file.unwrap(), magic);
        if (expect("shell: read(/vfs/boot/bin/shell) = ", status_of(read), Status::Ok)) {
            ok &= read.unwrap() == magic.size() && magic[0] == 0x7f && magic[1] == 'E' &&
                  magic[2] == 'L' && magic[3] == 'F';
            if (!(read.unwrap() == magic.size() && magic[0] == 0x7f && magic[1] == 'E' &&
                  magic[2] == 'L' && magic[3] == 'F')) {
                auto _ = ort::debug_line("shell: /vfs/boot/bin/shell is not an ELF FAILED");
            }
        } else {
            ok = false;
        }
        ok &=
            expect("shell: close(/vfs/boot/bin/shell) = ", fsio::close(file.unwrap()), Status::Ok);
    } else {
        ok = false;
    }

    // The writable mount: create a file in memfs's pool, write it, read it
    // back through a second connection -- three servers away for the bytes,
    // fshost forwarding every step.
    constexpr std::string_view TEXT{"written through /vfs/tmp"};
    auto const made =
        fsio::open("/vfs/tmp/hello", ours_io::PERM_WRITABLE | ours_io::Flags::FlagMaybeCreate);
    ok &= expect("shell: open(/vfs/tmp/hello, maybe_create) = ", status_of(made), Status::Ok);
    if (made.is_ok()) {
        auto const written =
            fsio::write(made.unwrap(), {reinterpret_cast<u8 const *>(TEXT.data()), TEXT.size()});
        ok &= expect("shell: write(/vfs/tmp/hello) = ", status_of(written), Status::Ok);
        ok &= written.is_ok() && written.unwrap() == TEXT.size();
        ok &= expect("shell: close(/vfs/tmp/hello) = ", fsio::close(made.unwrap()), Status::Ok);
    } else {
        ok = false;
    }
    auto const read_back = fsio::open("/vfs/tmp/hello", ours_io::Flags::PermReadBytes);
    ok &= expect("shell: open(/vfs/tmp/hello, read) = ", status_of(read_back), Status::Ok);
    if (read_back.is_ok()) {
        std::array<u8, 32> buffer{};
        auto const read = fsio::read(read_back.unwrap(), buffer);
        if (expect("shell: read(/vfs/tmp/hello) = ", status_of(read), Status::Ok)) {
            auto const got =
                std::string_view{reinterpret_cast<char const *>(buffer.data()), read.unwrap()};
            ok &= got == TEXT;
            if (got != TEXT) {
                auto _ = ort::debug_line("shell: /vfs/tmp/hello does not read back FAILED");
            }
        } else {
            ok = false;
        }
        ok &= expect("shell: close(/vfs/tmp/hello again) = ", fsio::close(read_back.unwrap()),
                     Status::Ok);
    } else {
        ok = false;
    }

    if (ok) {
        auto _ = ort::debug_line("shell: vfs checks passed");
    } else {
        auto _ = ort::debug_line("shell: vfs checks FAILED");
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
    static Uandle MADE[ROOM];
    u32 count = 0;
    auto status = Status::Ok;
    while (count + 2 <= ROOM) {
        status =
            ours::channel_create(ours::ChannelCreateOptions::None, &MADE[count], &MADE[count + 1]);
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
        ok &= ours::handle_close(MADE[count - 1]) == Status::Ok;
        ok &= ours::handle_close(MADE[count - 2]) == Status::Ok;
        ok &= expect("shell: channel_create after closing one = ",
                     ours::channel_create(ours::ChannelCreateOptions::None, &MADE[count - 2],
                                          &MADE[count - 1]),
                     Status::Ok);
    }
    for (u32 i = 0; i < count; ++i) {
        ok &= ours::handle_close(MADE[i]) == Status::Ok;
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
    // copied out, so when the copy fails the message is gone, and the next
    // read finds the channel empty.
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

/// Interrupt objects on physical lines, where this shell holds the IRQ root
/// (dev.dbg_1), from this one thread: what interrupt_create answers. No
/// device raises the line used, so no interrupt arrives here; the kernel's
/// interruptline.test raises one, and wakes a parked waiter with it.
///
///   - the lines used are asked for with interrupt_map, the root domain and
///     GSI 2, then GSI 3, as a driver would: a line is named by the kernel's
///     number for it, resolved from the domain and the source that name it;
///   - the timer's line, GSI 2 on q35, is the kernel's for good: interrupt_map
///     refuses its pin and interrupt_create its line, AccessDenied, with a
///     mode or without;
///   - GSI 3, COM2's line, is free, since QEMU has no COM2 with one -serial.
///     A create that cannot write its handle out fails (InvArgs) with the
///     line already taken, and the handle it drops destroys the object, which
///     gives the line back: the next create takes it, and a second answers
///     AlreadyBound;
///   - an object on a line cannot be triggered by hand (BadState), and once
///     destroyed a wait answers Cancelled;
///   - a destroy lets the line go though the handle stays open: a create
///     takes it again, and closing the destroyed object's handle leaves the
///     new owner the line; closing the owner's last handle lets it go;
///   - the I/O port root grants no line (BadType); a mode that names none is
///     InvArgs, and so is the retired RemapIrq bit, 0x1; a number no line has
///     is NoResource, a pin's GSI taken for a line included, since a line is
///     named by the kernel's own number for it; except 0xFFFFFFFF, which the
///     kernel keeps as no line at all: InvArgs, for interrupt_map as well. A
///     source no pin has is NoResource to interrupt_map, a domain no
///     controller registers too, the domain the kernel keeps as none is
///     InvArgs, and an |out| that faults fails it (InvArgs).
auto check_interrupt_lines(ours::Uandle const irq, ours::Uandle const ioport) -> bool {
    using ours::InterruptOptions;
    using ours::InterruptTriggerOptions;
    using ours::Status;
    using ours::Uandle;

    constexpr auto GSI_TIMER = u32{2};
    constexpr auto GSI_FREE = u32{3};
    constexpr auto NO_PIN = u32{200};
    constexpr auto NO_DOMAIN = u32{7};

    auto ok = true;
    // A line is named by the kernel's number for it, which no program
    // computes: ask for the two lines this check uses, as a driver would.
    constexpr auto UNTOUCHED = u32{0xA5A5'A5A5};
    auto refused = UNTOUCHED;
    auto free_line = u32{0};
    if (!expect("shell: interrupt_map(the root domain, GSI 2, the kernel's timer) = ",
                ours::interrupt_map(irq, ours::ROOT_INTERRUPT_DOMAIN, GSI_TIMER, &refused),
                Status::AccessDenied)) {
        return false;
    }
    if (refused != UNTOUCHED) {
        auto _ = ort::debug_line("shell: interrupt_map wrote its output though it refused");
        return false;
    }
    if (!expect("shell: interrupt_map(the root domain, GSI 3) = ",
                ours::interrupt_map(irq, ours::ROOT_INTERRUPT_DOMAIN, GSI_FREE, &free_line),
                Status::Ok)) {
        return false;
    }
    // GSI 2 is refused, so its line is found from its neighbour's: the kernel
    // numbers lines consecutively in the order of their pins, and GSI 2 and
    // GSI 3 are both pins on this machine.
    auto const timer_line = free_line - 1;
    auto first = Uandle::Invalid;
    auto second = Uandle::Invalid;
    ok &= expect("shell: interrupt_create(GSI 2, the timer's) = ",
                 ours::interrupt_create(irq, timer_line, InterruptOptions::None, &first),
                 Status::AccessDenied);
    ok &= expect("shell: interrupt_create(GSI 2, ModeLevelHigh) = ",
                 ours::interrupt_create(irq, timer_line, InterruptOptions::ModeLevelHigh, &first),
                 Status::AccessDenied);

    // The handle's value is written out last, once the line is taken and
    // unmasked, so an |out| that faults fails the create after the line is
    // the object's. Only the handle dropped on that path can give it back.
    ok &= expect("shell: interrupt_create(GSI 3, out = unmapped) = ",
                 ours::interrupt_create(irq, free_line, InterruptOptions::None,
                                        reinterpret_cast<Uandle *>(UNMAPPED)),
                 Status::InvArgs);
    if (!expect("shell: interrupt_create(GSI 3) = ",
                ours::interrupt_create(irq, free_line, InterruptOptions::None, &first),
                Status::Ok)) {
        return false;
    }
    ok &= expect("shell: interrupt_create(GSI 3, taken) = ",
                 ours::interrupt_create(irq, free_line, InterruptOptions::None, &second),
                 Status::AlreadyBound);
    ok &=
        expect("shell: interrupt_trigger(on a line) = ",
               ours::interrupt_trigger(first, InterruptTriggerOptions::None, 1), Status::BadState);
    ok &= expect("shell: interrupt_destroy(on a line) = ", ours::interrupt_destroy(first),
                 Status::Ok);
    ok &= expect("shell: interrupt_wait(on a line, destroyed) = ",
                 ours::interrupt_wait(first, nullptr), Status::Cancelled);

    // The destroyed object's handle is still open; its line is free.
    if (expect("shell: interrupt_create(GSI 3, after the destroy) = ",
               ours::interrupt_create(irq, free_line, InterruptOptions::ModeEdgeHigh, &second),
               Status::Ok)) {
        ok &=
            expect("shell: close(the destroyed object) = ", ours::handle_close(first), Status::Ok);
        auto third = Uandle::Invalid;
        ok &= expect("shell: interrupt_create(GSI 3, the new owner's) = ",
                     ours::interrupt_create(irq, free_line, InterruptOptions::None, &third),
                     Status::AlreadyBound);
        ok &= expect("shell: close(the new owner) = ", ours::handle_close(second), Status::Ok);
        if (expect("shell: interrupt_create(GSI 3, after the close) = ",
                   ours::interrupt_create(irq, free_line, InterruptOptions::None, &third),
                   Status::Ok)) {
            ok &= ours::handle_close(third) == Status::Ok;
        } else {
            ok = false;
        }
    } else {
        ok = false;
        auto _ = ours::handle_close(first);
    }

    auto unused = Uandle::Invalid;
    ok &= expect("shell: interrupt_create(the I/O port root) = ",
                 ours::interrupt_create(ioport, free_line, InterruptOptions::None, &unused),
                 Status::BadType);
    ok &= expect(
        "shell: interrupt_create(a mode that names none) = ",
        ours::interrupt_create(irq, free_line, static_cast<InterruptOptions>(5U << 1), &unused),
        Status::InvArgs);
    ok &= expect("shell: interrupt_create(a line no pin is) = ",
                 ours::interrupt_create(irq, 200, InterruptOptions::None, &unused),
                 Status::NoResource);
    // 0x1 was RemapIrq, retired: the bit is reserved and not reused, so a
    // create that sets it is refused (InvArgs), whatever else it asks.
    ok &=
        expect("shell: interrupt_create(the retired RemapIrq bit) = ",
               ours::interrupt_create(irq, free_line, static_cast<InterruptOptions>(0x1), &unused),
               Status::InvArgs);
    // A source no pin has names no line, and a domain no controller registers
    // resolves nothing; 0xFFFFFFFF, which the kernel keeps as no source (or no
    // domain) at all, is refused.
    auto number = u32{0};
    ok &= expect("shell: interrupt_map(the root domain, a source no pin has) = ",
                 ours::interrupt_map(irq, ours::ROOT_INTERRUPT_DOMAIN, NO_PIN, &number),
                 Status::NoResource);
    ok &= expect("shell: interrupt_map(a domain no controller registers) = ",
                 ours::interrupt_map(irq, NO_DOMAIN, GSI_FREE, &number), Status::NoResource);
    ok &= expect("shell: interrupt_map(the root domain, 0xFFFFFFFF) = ",
                 ours::interrupt_map(irq, ours::ROOT_INTERRUPT_DOMAIN, 0xFFFF'FFFF, &number),
                 Status::InvArgs);
    ok &= expect("shell: interrupt_map(0xFFFFFFFF, GSI 3) = ",
                 ours::interrupt_map(irq, 0xFFFF'FFFF, GSI_FREE, &number), Status::InvArgs);
    // The kernel's line is refused after the other checks, not before: a
    // caller with no handle, or the wrong root, or a mode that names none,
    // is told that, and learns nothing about the line.
    ok &= expect(
        "shell: interrupt_map(the root domain, GSI 2, no handle) = ",
        ours::interrupt_map(Uandle::Invalid, ours::ROOT_INTERRUPT_DOMAIN, GSI_TIMER, &number),
        Status::BadHandle);
    ok &= expect("shell: interrupt_map(the root domain, GSI 2, the I/O port root) = ",
                 ours::interrupt_map(ioport, ours::ROOT_INTERRUPT_DOMAIN, GSI_TIMER, &number),
                 Status::BadType);
    ok &=
        expect("shell: interrupt_create(the timer's line, no handle) = ",
               ours::interrupt_create(Uandle::Invalid, timer_line, InterruptOptions::None, &unused),
               Status::BadHandle);
    ok &= expect("shell: interrupt_create(the timer's line, the I/O port root) = ",
                 ours::interrupt_create(ioport, timer_line, InterruptOptions::None, &unused),
                 Status::BadType);
    ok &= expect(
        "shell: interrupt_create(the timer's line, a mode that names none) = ",
        ours::interrupt_create(irq, timer_line, static_cast<InterruptOptions>(5U << 1), &unused),
        Status::InvArgs);
    // interrupt_map writes its answer last: an |out| that faults fails a call
    // that found the line, and takes nothing.
    ok &= expect("shell: interrupt_map(the root domain, GSI 3, out = unmapped) = ",
                 ours::interrupt_map(irq, ours::ROOT_INTERRUPT_DOMAIN, GSI_FREE,
                                     reinterpret_cast<u32 *>(UNMAPPED)),
                 Status::InvArgs);
    ok &= expect("shell: interrupt_map(the root domain, GSI 3) = ",
                 ours::interrupt_map(irq, ours::ROOT_INTERRUPT_DOMAIN, GSI_FREE, &number),
                 Status::Ok);
    // A line is named by the kernel's number for it, never the pin's GSI.
    // COM2's pin is GSI 3 on q35; taken for a line, it names none: interrupt_map
    // is what turns the platform's number for a pin into one.
    ok &= expect("shell: interrupt_create(a GSI, not mapped) = ",
                 ours::interrupt_create(irq, GSI_FREE, InterruptOptions::None, &unused),
                 Status::NoResource);
    ok &= expect("shell: interrupt_create(line 0xFFFFFFFF) = ",
                 ours::interrupt_create(irq, 0xFFFF'FFFF, InterruptOptions::None, &unused),
                 Status::InvArgs);
    return ok;
}

/// Interrupt objects (interrupt_create, interrupt_trigger, interrupt_wait,
/// interrupt_destroy), on a virtual interrupt and from this one thread: every
/// wait below finds an interrupt pending, or the object destroyed, and so
/// returns without parking. Waking a parked waiter -- by a trigger, or with
/// Cancelled by a destroy -- needs a second thread, which a program cannot
/// start yet (no thread_start); the kernel's interrupt.test covers it.
///
/// Where userboot started this shell (dev.dbg_1), check_interrupt_lines()
/// adds the physical lines to the same verdict, with the IRQ root userboot
/// hands over: the kernel mints it on every boot, so a missing one fails
/// the check rather than skipping it. Where the root started it (dev), the
/// root keeps the IRQ root, and the physical lines are skipped.
///
/// What it checks, in the order the kernel decides it:
///   - interrupt_create refuses an option it does not know (InvArgs), Virtual
///     with an option of a physical line (InvArgs), and a physical line
///     without a resource (BadHandle);
///   - a wait returns the time of the trigger it takes, and triggers before
///     that wait fold into the first;
///   - a trigger after a wait took one is pending for the next wait, which
///     returns at once with its time: the wait that would ack the first
///     takes the second. A trigger at time 0 counts as one;
///   - a wait may leave out the time; one whose time cannot be written
///     answers InvArgs, with the interrupt taken;
///   - trigger needs Signal and None for its options, wait needs Wait, and
///     a handle to something else is BadType for all three calls;
///   - after interrupt_destroy, wait and trigger answer Cancelled, and a
///     second destroy is Ok.
auto check_interrupt(ours::Uandle const irq, ours::Uandle const ioport, bool const component)
    -> void {
    using ours::InterruptOptions;
    using ours::InterruptTriggerOptions;
    using ours::Status;
    using ours::Uandle;

    auto ok = true;
    Uandle interrupt = Uandle::Invalid;
    ok &= expect("shell: interrupt_create(an unknown option) = ",
                 ours::interrupt_create(Uandle::Invalid, 0, static_cast<InterruptOptions>(0x8000),
                                        &interrupt),
                 Status::InvArgs);
    ok &= expect("shell: interrupt_create(Virtual, a mode) = ",
                 ours::interrupt_create(Uandle::Invalid, 0,
                                        InterruptOptions::Virtual | InterruptOptions::ModeEdgeHigh,
                                        &interrupt),
                 Status::InvArgs);
    ok &= expect("shell: interrupt_create(a physical line, no resource) = ",
                 ours::interrupt_create(Uandle::Invalid, 3, InterruptOptions::None, &interrupt),
                 Status::BadHandle);
    if (component) {
        auto _ = ort::debug_line(
            "shell: interrupt checks on physical lines skipped (the root keeps the IRQ root)");
    } else {
        ok &= check_interrupt_lines(irq, ioport);
    }
    if (!expect("shell: interrupt_create(Virtual) = ",
                ours::interrupt_create(Uandle::Invalid, 0, InterruptOptions::Virtual, &interrupt),
                Status::Ok)) {
        auto _ = ort::debug_line("shell: interrupt checks FAILED");
        return;
    }

    // Whether a wait on |interrupt| returns Ok with |want|, the time of the
    // trigger it should take. Filled with another value first, so that a
    // time the kernel failed to write cannot pass.
    auto const takes = [&interrupt](std::string_view const what, ours::Instant const want) {
        auto got = ours::Instant{-1};
        if (!expect(what, ours::interrupt_wait(interrupt, &got), Status::Ok)) {
            return false;
        }
        if (got != want) {
            auto _ = ort::debug_line("shell: interrupt_wait wrote another time");
            return false;
        }
        return true;
    };

    ok &= expect("shell: interrupt_trigger(an option) = ",
                 ours::interrupt_trigger(interrupt, static_cast<InterruptTriggerOptions>(1), 1),
                 Status::InvArgs);

    // Two triggers before a wait: one interrupt, with the first time.
    ok &=
        expect("shell: interrupt_trigger(1001) = ",
               ours::interrupt_trigger(interrupt, InterruptTriggerOptions::None, 1001), Status::Ok);
    ok &=
        expect("shell: interrupt_trigger(1002) = ",
               ours::interrupt_trigger(interrupt, InterruptTriggerOptions::None, 1002), Status::Ok);
    ok &= takes("shell: interrupt_wait(triggered at 1001, 1002) = ", 1001);

    // Taken, so the object needs an ack; the trigger that comes meanwhile
    // is pending, and the next wait takes it at once.
    ok &=
        expect("shell: interrupt_trigger(2001) = ",
               ours::interrupt_trigger(interrupt, InterruptTriggerOptions::None, 2001), Status::Ok);
    ok &= takes("shell: interrupt_wait(triggered at 2001 before the ack) = ", 2001);
    ok &= expect("shell: interrupt_trigger(0) = ",
                 ours::interrupt_trigger(interrupt, InterruptTriggerOptions::None, 0), Status::Ok);
    ok &= takes("shell: interrupt_wait(triggered at 0) = ", 0);

    ok &=
        expect("shell: interrupt_trigger(3001) = ",
               ours::interrupt_trigger(interrupt, InterruptTriggerOptions::None, 3001), Status::Ok);
    ok &= expect("shell: interrupt_wait(no time asked for) = ",
                 ours::interrupt_wait(interrupt, nullptr), Status::Ok);

    // Handles without the right each call asks for.
    constexpr auto WITHOUT_SIGNAL = ours::Rights::Basic | ours::Rights::Io;
    constexpr auto WITHOUT_WAIT =
        (ours::Rights::Basic & ~ours::Rights::Wait) | ours::Rights::Io | ours::Rights::Signal;
    Uandle no_signal = Uandle::Invalid;
    if (expect("shell: interrupt handle_duplicate(without Signal) = ",
               ours::handle_duplicate(interrupt, WITHOUT_SIGNAL, &no_signal), Status::Ok)) {
        ok &= expect("shell: interrupt_trigger(no Signal) = ",
                     ours::interrupt_trigger(no_signal, InterruptTriggerOptions::None, 1),
                     Status::AccessDenied);
        ok &= ours::handle_close(no_signal) == Status::Ok;
    } else {
        ok = false;
    }
    Uandle no_wait = Uandle::Invalid;
    if (expect("shell: interrupt handle_duplicate(without Wait) = ",
               ours::handle_duplicate(interrupt, WITHOUT_WAIT, &no_wait), Status::Ok)) {
        ok &= expect("shell: interrupt_wait(no Wait) = ", ours::interrupt_wait(no_wait, nullptr),
                     Status::AccessDenied);
        ok &= ours::handle_close(no_wait) == Status::Ok;
    } else {
        ok = false;
    }

    // Something that is not an interrupt.
    Uandle a = Uandle::Invalid;
    Uandle b = Uandle::Invalid;
    if (expect("shell: interrupt channel_create = ",
               ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        ok &= expect("shell: interrupt_trigger(a channel) = ",
                     ours::interrupt_trigger(a, InterruptTriggerOptions::None, 1), Status::BadType);
        ok &= expect("shell: interrupt_wait(a channel) = ", ours::interrupt_wait(a, nullptr),
                     Status::BadType);
        ok &= expect("shell: interrupt_destroy(a channel) = ", ours::interrupt_destroy(a),
                     Status::BadType);
        ok &= ours::handle_close(a) == Status::Ok && ours::handle_close(b) == Status::Ok;
    } else {
        ok = false;
    }

    // A time that cannot be written: the interrupt is taken all the same,
    // so the object is left needing an ack with nothing pending, and a wait
    // now would park. Destroyed next, nothing waits again.
    ok &=
        expect("shell: interrupt_trigger(4001) = ",
               ours::interrupt_trigger(interrupt, InterruptTriggerOptions::None, 4001), Status::Ok);
    ok &= expect("shell: interrupt_wait(time to the kernel half) = ",
                 ours::interrupt_wait(interrupt, reinterpret_cast<ours::Instant *>(KERNEL)),
                 Status::InvArgs);

    ok &= expect("shell: interrupt_destroy = ", ours::interrupt_destroy(interrupt), Status::Ok);
    ok &= expect("shell: interrupt_wait(destroyed) = ", ours::interrupt_wait(interrupt, nullptr),
                 Status::Cancelled);
    ok &= expect("shell: interrupt_trigger(destroyed) = ",
                 ours::interrupt_trigger(interrupt, InterruptTriggerOptions::None, 1),
                 Status::Cancelled);
    ok &= expect("shell: interrupt_destroy(again) = ", ours::interrupt_destroy(interrupt),
                 Status::Ok);
    ok &= expect("shell: close(interrupt) = ", ours::handle_close(interrupt), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: interrupt checks passed");
    } else {
        auto _ = ort::debug_line("shell: interrupt checks FAILED");
    }
}

/// A virtual interrupt's Idle signal (<ours/signals.hpp>), seen through
/// object_wait_one and object_wait_async, then the loop of a driver for a
/// controller behind a root line, one port for all of it:
///   - Idle is up on a new interrupt, down after a trigger and after the wait
///     that takes the interrupt, and up for good after a destroy, which a
///     refused trigger leaves up;
///   - the parent is bound to the port and its Interrupt packet is read; the
///     child is triggered, Idle is waited for with object_wait_async on the
///     same port, and no packet comes while the client has not acked;
///   - the client's handle (Wait, Read, Write and Transfer) is bound to a
///     second port; its packet is read and acked, and the ack brings the
///     SignalOne packet for Idle with the driver's key, observed Idle;
///   - the client's handle cannot trigger (AccessDenied) or be duplicated
///     (AccessDenied);
///   - a destroy by the client brings the SignalOne packet again, and the
///     driver's next trigger answers Cancelled.
auto check_interrupt_idle() -> void {
    using ours::InterruptBindOptions;
    using ours::InterruptOptions;
    using ours::InterruptTriggerOptions;
    using ours::PacketType;
    using ours::PortCreateOptions;
    using ours::PortPacket;
    using ours::Signals;
    using ours::Status;
    using ours::Uandle;
    using ours::WaitAsyncOptions;

    constexpr auto PARENT_KEY = u64{0x1A7E'0000'0000'0011};
    constexpr auto CHILD_KEY = u64{0x1A7E'0000'0000'0012};
    constexpr auto CLIENT_KEY = u64{0x1A7E'0000'0000'0013};

    auto ok = true;
    Uandle parent = Uandle::Invalid;
    Uandle child = Uandle::Invalid;
    Uandle driver_port = Uandle::Invalid;
    Uandle client_port = Uandle::Invalid;
    // Closes what has been made so far, on any way out.
    auto const close_all = [&]() {
        for (auto *const handle: {&parent, &child, &driver_port, &client_port}) {
            if (*handle != Uandle::Invalid) {
                auto _ = ours::handle_close(*handle);
                *handle = Uandle::Invalid;
            }
        }
    };
    if (!expect("shell: interrupt idle interrupt_create(parent) = ",
                ours::interrupt_create(Uandle::Invalid, 0, InterruptOptions::Virtual, &parent),
                Status::Ok) ||
        !expect("shell: interrupt idle interrupt_create(child) = ",
                ours::interrupt_create(Uandle::Invalid, 0, InterruptOptions::Virtual, &child),
                Status::Ok) ||
        !expect("shell: interrupt idle port_create(driver) = ",
                ours::port_create(PortCreateOptions::BindToInterrupt, &driver_port), Status::Ok) ||
        !expect("shell: interrupt idle port_create(client) = ",
                ours::port_create(PortCreateOptions::BindToInterrupt, &client_port), Status::Ok)) {
        close_all();
        auto _ = ort::debug_line("shell: interrupt idle checks FAILED");
        return;
    }

    // Whether a poll for Idle on |handle| finds it up (Ok, and the signal in
    // what was observed) or down (TimeOut).
    auto const idle = [](std::string_view const what, Uandle const handle, bool const up) {
        auto seen = Signals::None;
        auto const status = ours::object_wait_one(handle, Signals::Idle, 0, &seen);
        if (!expect(what, status, up ? Status::Ok : Status::TimeOut)) {
            return false;
        }
        if (up && !(seen & Signals::Idle)) {
            auto _ = ort::debug_line("shell: object_wait_one did not report Idle");
            return false;
        }
        return true;
    };
    auto const trigger = [](Uandle const handle, ours::Instant const at) {
        return ours::interrupt_trigger(handle, InterruptTriggerOptions::None, at);
    };
    // The next packet of |port|, of |type| and |key|, with |signals| observed
    // if it is a SignalOne. Filled with 0xFF first.
    auto const reads = [](std::string_view const what, Uandle const port, PacketType const type,
                          u64 const key, Signals const observed) {
        PortPacket got;
        std::memset(&got, 0xFF, sizeof(got));
        if (!expect(what, ours::port_wait(port, 0, &got), Status::Ok)) {
            return false;
        }
        auto right = got.type == type && got.key == key && got.status == Status::Ok;
        if (type == PacketType::SignalOne) {
            right = right && !!(got.signal.observed & observed);
        }
        if (!right) {
            auto _ = ort::debug_line("shell: port_wait read another packet");
        }
        return right;
    };
    auto const empty = [](std::string_view const what, Uandle const port) {
        PortPacket got;
        return expect(what, ours::port_wait(port, 0, &got), Status::TimeOut);
    };

    // The signal on its own, through the one interrupt an unbound wait takes.
    ok &= idle("shell: object_wait_one(Idle, a new interrupt) = ", child, true);
    ok &= expect("shell: interrupt_trigger(idle, 1) = ", trigger(child, 1), Status::Ok);
    ok &= idle("shell: object_wait_one(Idle, triggered) = ", child, false);
    ok &= expect("shell: interrupt_trigger(idle, 2) = ", trigger(child, 2), Status::Ok);
    ok &= idle("shell: object_wait_one(Idle, triggered again) = ", child, false);
    ok &= expect("shell: interrupt_wait(idle, takes) = ", ours::interrupt_wait(child, nullptr),
                 Status::Ok);
    ok &= idle("shell: object_wait_one(Idle, taken, not acked) = ", child, false);
    ok &= expect("shell: interrupt_destroy(idle) = ", ours::interrupt_destroy(child), Status::Ok);
    ok &= idle("shell: object_wait_one(Idle, destroyed) = ", child, true);
    ok &= expect("shell: interrupt_trigger(idle, destroyed) = ", trigger(child, 3),
                 Status::Cancelled);
    ok &= idle("shell: object_wait_one(Idle, destroyed, refused trigger) = ", child, true);
    ok &= ours::handle_close(child) == Status::Ok;
    child = Uandle::Invalid;

    // The driver's loop. The parent stands for a root line.
    if (!expect("shell: interrupt idle interrupt_create(child, again) = ",
                ours::interrupt_create(Uandle::Invalid, 0, InterruptOptions::Virtual, &child),
                Status::Ok)) {
        close_all();
        auto _ = ort::debug_line("shell: interrupt idle checks FAILED");
        return;
    }
    ok &= expect("shell: interrupt_bind(parent) = ",
                 ours::interrupt_bind(parent, driver_port, PARENT_KEY, InterruptBindOptions::Bind),
                 Status::Ok);
    ok &= expect("shell: interrupt_trigger(parent) = ", trigger(parent, 10), Status::Ok);
    ok &= reads("shell: port_wait(the parent's packet) = ", driver_port, PacketType::Interrupt,
                PARENT_KEY, Signals::None);

    // The pin fires: the driver triggers the child and waits for Idle on the
    // port it already has.
    ok &= expect("shell: interrupt_trigger(child) = ", trigger(child, 20), Status::Ok);
    ok &= expect("shell: object_wait_async(child, Idle) = ",
                 ours::object_wait_async(child, driver_port, CHILD_KEY, Signals::Idle,
                                         WaitAsyncOptions::None),
                 Status::Ok);
    ok &= empty("shell: port_wait(the client has not acked) = ", driver_port);

    // The client's handle: Wait, Read, Write and Transfer.
    constexpr auto CLIENT_RIGHTS =
        ours::Rights::Wait | ours::Rights::Read | ours::Rights::Write | ours::Rights::Transfer;
    Uandle client = Uandle::Invalid;
    if (expect("shell: handle_duplicate(child, the client's rights) = ",
               ours::handle_duplicate(child, CLIENT_RIGHTS, &client), Status::Ok)) {
        Uandle again = Uandle::Invalid;
        ok &= expect("shell: interrupt_trigger(client's handle) = ", trigger(client, 30),
                     Status::AccessDenied);
        ok &= expect("shell: handle_duplicate(client's handle) = ",
                     ours::handle_duplicate(client, ours::Rights::SameRights, &again),
                     Status::AccessDenied);
        if (again != Uandle::Invalid) {
            // Only if the refusal failed: do not leave the handle behind.
            ok &= ours::handle_close(again) == Status::Ok;
        }
        ok &= expect(
            "shell: interrupt_bind(client's handle) = ",
            ours::interrupt_bind(client, client_port, CLIENT_KEY, InterruptBindOptions::Bind),
            Status::Ok);
        ok &= reads("shell: port_wait(the client's packet) = ", client_port, PacketType::Interrupt,
                    CLIENT_KEY, Signals::None);
        ok &= empty("shell: port_wait(still not acked) = ", driver_port);
        ok &= expect("shell: interrupt_ack(client's handle) = ", ours::interrupt_ack(client),
                     Status::Ok);
        ok &= reads("shell: port_wait(Idle, after the client's ack) = ", driver_port,
                    PacketType::SignalOne, CHILD_KEY, Signals::Idle);
        ok &= idle("shell: object_wait_one(Idle, acked) = ", child, true);

        // The pin fires again, and the client leaves without an ack.
        ok &= expect("shell: interrupt_trigger(child, again) = ", trigger(child, 40), Status::Ok);
        ok &= idle("shell: object_wait_one(Idle, triggered, again) = ", child, false);
        ok &= expect("shell: object_wait_async(child, Idle, again) = ",
                     ours::object_wait_async(child, driver_port, CHILD_KEY, Signals::Idle,
                                             WaitAsyncOptions::None),
                     Status::Ok);
        ok &= empty("shell: port_wait(owed again) = ", driver_port);
        // The client's other handle closes without an ack: nothing is raised.
        Uandle leaver = Uandle::Invalid;
        if (expect("shell: handle_duplicate(child, a second client handle, to leave) = ",
                   ours::handle_duplicate(child, CLIENT_RIGHTS, &leaver), Status::Ok)) {
            ok &= ours::handle_close(leaver) == Status::Ok;
            ok &= idle("shell: object_wait_one(Idle, a handle closed without an ack) = ", child,
                       false);
            ok &= empty("shell: port_wait(a handle closed without an ack) = ", driver_port);
        } else {
            ok = false;
        }
        ok &=
            expect("shell: interrupt_destroy(client's handle) = ", ours::interrupt_destroy(client),
                   Status::Ok);
        ok &= reads("shell: port_wait(Idle, after the client's destroy) = ", driver_port,
                    PacketType::SignalOne, CHILD_KEY, Signals::Idle);
        ok &= expect("shell: interrupt_trigger(child, destroyed by the client) = ",
                     trigger(child, 50), Status::Cancelled);
        ok &= ours::handle_close(client) == Status::Ok;
    } else {
        ok = false;
    }

    ok &= expect("shell: interrupt_ack(parent) = ", ours::interrupt_ack(parent), Status::Ok);
    for (auto const handle: {parent, child, driver_port, client_port}) {
        ok &= ours::handle_close(handle) == Status::Ok;
    }

    if (ok) {
        auto _ = ort::debug_line("shell: interrupt idle checks passed");
    } else {
        auto _ = ort::debug_line("shell: interrupt idle checks FAILED");
    }
}

/// Interrupts bound to a port (interrupt_bind, interrupt_ack), on virtual
/// interrupts and from this one thread: every port_wait below is a poll.
///
/// What it checks, in the order the kernel decides it:
///   - a bind is refused for unknown options (InvArgs), a port made without
///     BindToInterrupt or a handle of the wrong kind (BadType), and handles
///     without Read on the interrupt or Write on the port (AccessDenied); an
///     ack needs a bound interrupt (BadState) and Write;
///   - bound, a wait answers BadState and a second bind AlreadyBound;
///   - the first trigger sends a packet at once, with its time and the key
///     given to the bind; triggers after it send nothing until the ack,
///     which sends them at once as one packet, with the first one's time;
///   - an ack with nothing pending sends nothing, and a second is Ok;
///   - an interrupt packet is read before a port_queue packet queued ahead
///     of it;
///   - an ack that would send the next packet while the last is still
///     queued answers BadState, and the interrupts since fold into it;
///   - an ack with nothing pending, before the read, leaves the packet
///     queued, and a trigger after it folds into that packet too;
///   - an unbind takes back a packet still queued, and the interrupt is
///     pending again for a wait; a second unbind answers NotFound;
///   - a bind sends an interrupt already pending at once;
///   - a destroy takes back a packet still queued, so none stays on the
///     port, and answers NotFound once port_wait has taken the packet.
auto check_interrupt_port() -> void {
    using ours::InterruptBindOptions;
    using ours::InterruptOptions;
    using ours::InterruptTriggerOptions;
    using ours::PacketType;
    using ours::PortCreateOptions;
    using ours::PortPacket;
    using ours::Status;
    using ours::Uandle;

    constexpr auto KEY = u64{0x1A7E'0000'0000'0001};
    constexpr auto OTHER_KEY = u64{0x1A7E'0000'0000'0002};

    auto ok = true;
    Uandle interrupt = Uandle::Invalid;
    Uandle other = Uandle::Invalid;
    Uandle port = Uandle::Invalid;
    Uandle plain_port = Uandle::Invalid;
    if (!expect("shell: interrupt port interrupt_create = ",
                ours::interrupt_create(Uandle::Invalid, 0, InterruptOptions::Virtual, &interrupt),
                Status::Ok) ||
        !expect("shell: interrupt port interrupt_create(other) = ",
                ours::interrupt_create(Uandle::Invalid, 0, InterruptOptions::Virtual, &other),
                Status::Ok) ||
        !expect("shell: port_create(BindToInterrupt) = ",
                ours::port_create(PortCreateOptions::BindToInterrupt, &port), Status::Ok) ||
        !expect("shell: interrupt port port_create(None) = ",
                ours::port_create(PortCreateOptions::None, &plain_port), Status::Ok)) {
        auto _ = ort::debug_line("shell: interrupt port checks FAILED");
        return;
    }

    auto const trigger = [](Uandle const handle, ours::Instant const at) {
        return ours::interrupt_trigger(handle, InterruptTriggerOptions::None, at);
    };
    auto const bind = [&port](Uandle const handle, u64 const key) {
        return ours::interrupt_bind(handle, port, key, InterruptBindOptions::Bind);
    };
    // Whether a poll of |port| reads an Interrupt packet with |key| and the
    // time |at|. Filled with 0xFF first, so a field the kernel failed to
    // write cannot pass.
    auto const reads = [&port](std::string_view const what, u64 const key, ours::Instant const at) {
        PortPacket got;
        std::memset(&got, 0xFF, sizeof(got));
        if (!expect(what, ours::port_wait(port, 0, &got), Status::Ok)) {
            return false;
        }
        auto const right = got.key == key && got.type == PacketType::Interrupt &&
                           got.status == Status::Ok && got.interrupt.timestamp == at &&
                           got.interrupt.reserved0 == 0 && got.interrupt.reserved1 == 0 &&
                           got.interrupt.reserved2 == 0;
        if (!right) {
            auto _ = ort::debug_line("shell: port_wait read another interrupt packet");
        }
        return right;
    };
    auto const empty = [&port](std::string_view const what) {
        PortPacket got;
        return expect(what, ours::port_wait(port, 0, &got), Status::TimeOut);
    };

    // Refusals, before anything is bound.
    ok &= expect("shell: interrupt_bind(an unknown option) = ",
                 ours::interrupt_bind(interrupt, port, KEY, static_cast<InterruptBindOptions>(2)),
                 Status::InvArgs);
    ok &= expect("shell: interrupt_bind(a port without BindToInterrupt) = ",
                 ours::interrupt_bind(interrupt, plain_port, KEY, InterruptBindOptions::Bind),
                 Status::BadType);
    ok &= expect("shell: interrupt_bind(a port for the interrupt) = ", bind(port, KEY),
                 Status::BadType);
    ok &= expect("shell: interrupt_ack(unbound) = ", ours::interrupt_ack(interrupt),
                 Status::BadState);
    constexpr auto WITHOUT_IO = ours::Rights::Basic | ours::Rights::Signal;
    Uandle no_io = Uandle::Invalid;
    if (expect("shell: interrupt port handle_duplicate(without Read and Write) = ",
               ours::handle_duplicate(interrupt, WITHOUT_IO, &no_io), Status::Ok)) {
        ok &= expect("shell: interrupt_bind(no Read) = ", bind(no_io, KEY), Status::AccessDenied);
        ok &= expect("shell: interrupt_ack(no Write) = ", ours::interrupt_ack(no_io),
                     Status::AccessDenied);
        ok &= ours::handle_close(no_io) == Status::Ok;
    } else {
        ok = false;
    }
    constexpr auto WITHOUT_WRITE = (ours::Rights::Basic & ~ours::Rights::Wait) | ours::Rights::Read;
    Uandle read_only_port = Uandle::Invalid;
    if (expect("shell: interrupt port handle_duplicate(port without Write) = ",
               ours::handle_duplicate(port, WITHOUT_WRITE, &read_only_port), Status::Ok)) {
        ok &=
            expect("shell: interrupt_bind(a port without Write) = ",
                   ours::interrupt_bind(interrupt, read_only_port, KEY, InterruptBindOptions::Bind),
                   Status::AccessDenied);
        ok &= ours::handle_close(read_only_port) == Status::Ok;
    } else {
        ok = false;
    }

    // Bound: packets replace waits.
    ok &= expect("shell: interrupt_bind = ", bind(interrupt, KEY), Status::Ok);
    ok &= expect("shell: interrupt_bind(again) = ", bind(interrupt, KEY), Status::AlreadyBound);
    ok &= expect("shell: interrupt_wait(bound) = ", ours::interrupt_wait(interrupt, nullptr),
                 Status::BadState);
    ok &= empty("shell: port_wait(bound, nothing triggered) = ");

    // The first trigger sends; the two after it wait for the ack, which
    // sends them as one packet with the first one's time.
    ok &= expect("shell: interrupt_trigger(1001) = ", trigger(interrupt, 1001), Status::Ok);
    ok &= expect("shell: interrupt_trigger(1002) = ", trigger(interrupt, 1002), Status::Ok);
    ok &= reads("shell: port_wait(triggered at 1001) = ", KEY, 1001);
    ok &= expect("shell: interrupt_trigger(1003) = ", trigger(interrupt, 1003), Status::Ok);
    ok &= empty("shell: port_wait(1002 and 1003 wait for the ack) = ");
    ok &= expect("shell: interrupt_ack(retriggers) = ", ours::interrupt_ack(interrupt), Status::Ok);
    ok &= reads("shell: port_wait(sent by the ack) = ", KEY, 1002);
    ok &= expect("shell: interrupt_ack = ", ours::interrupt_ack(interrupt), Status::Ok);
    ok &= expect("shell: interrupt_ack(nothing outstanding) = ", ours::interrupt_ack(interrupt),
                 Status::Ok);
    ok &= empty("shell: port_wait(acked, nothing pending) = ");

    // Interrupt packets are read first.
    PortPacket user{};
    user.key = OTHER_KEY;
    ok &= expect("shell: interrupt port port_queue = ", ours::port_queue(port, &user), Status::Ok);
    ok &= expect("shell: interrupt_trigger(3001) = ", trigger(interrupt, 3001), Status::Ok);
    ok &= reads("shell: port_wait(interrupt before user) = ", KEY, 3001);
    PortPacket got;
    if (expect("shell: port_wait(then the user packet) = ", ours::port_wait(port, 0, &got),
               Status::Ok)) {
        if (got.type != PacketType::User || got.key != OTHER_KEY) {
            ok = false;
            auto _ = ort::debug_line("shell: port_wait read another user packet");
        }
    } else {
        ok = false;
    }
    ok &= expect("shell: interrupt_ack(after 3001) = ", ours::interrupt_ack(interrupt), Status::Ok);

    // An ack before the packet was read, with another interrupt to send.
    ok &= expect("shell: interrupt_trigger(4001) = ", trigger(interrupt, 4001), Status::Ok);
    ok &= expect("shell: interrupt_trigger(4002) = ", trigger(interrupt, 4002), Status::Ok);
    ok &= expect("shell: interrupt_ack(packet unread) = ", ours::interrupt_ack(interrupt),
                 Status::BadState);
    ok &= reads("shell: port_wait(the unread packet) = ", KEY, 4001);
    ok &= expect("shell: interrupt_ack(after 4001) = ", ours::interrupt_ack(interrupt), Status::Ok);
    ok &= empty("shell: port_wait(4002 folded into 4001) = ");

    // An ack with nothing pending, before the read: the packet stays
    // queued, and the trigger after it, though the ack ended the wait for
    // one, folds into it.
    ok &= expect("shell: interrupt_trigger(4501) = ", trigger(interrupt, 4501), Status::Ok);
    ok &= expect("shell: interrupt_ack(4501 unread, nothing pending) = ",
                 ours::interrupt_ack(interrupt), Status::Ok);
    ok &= expect("shell: interrupt_trigger(4502) = ", trigger(interrupt, 4502), Status::Ok);
    ok &= reads("shell: port_wait(4502 folded into the unread 4501) = ", KEY, 4501);
    ok &= empty("shell: port_wait(nothing after 4501) = ");
    ok &= expect("shell: interrupt_ack(after 4501) = ", ours::interrupt_ack(interrupt), Status::Ok);

    // Unbind with a packet queued: taken back, and pending for a wait.
    ok &= expect("shell: interrupt_trigger(5001) = ", trigger(interrupt, 5001), Status::Ok);
    ok &=
        expect("shell: interrupt_bind(Unbind) = ",
               ours::interrupt_bind(interrupt, port, 0, InterruptBindOptions::Unbind), Status::Ok);
    ok &= empty("shell: port_wait(unbound) = ");
    ok &= expect("shell: interrupt_bind(Unbind again) = ",
                 ours::interrupt_bind(interrupt, port, 0, InterruptBindOptions::Unbind),
                 Status::NotFound);
    auto at = ours::Instant{-1};
    if (!expect("shell: interrupt_wait(after the unbind) = ", ours::interrupt_wait(interrupt, &at),
                Status::Ok) ||
        at != 5001) {
        ok = false;
        auto _ = ort::debug_line("shell: interrupt_wait did not take the packet taken back");
    }

    // A bind sends an interrupt that is already pending.
    ok &= expect("shell: interrupt_trigger(other, 6001) = ", trigger(other, 6001), Status::Ok);
    ok &= expect("shell: interrupt_bind(other, pending) = ", bind(other, OTHER_KEY), Status::Ok);
    ok &= reads("shell: port_wait(sent by the bind) = ", OTHER_KEY, 6001);

    // A destroy takes back a packet still queued.
    ok &= expect("shell: interrupt_trigger(other, 7001) = ", trigger(other, 7001), Status::Ok);
    ok &= expect("shell: interrupt_ack(other) = ", ours::interrupt_ack(other), Status::Ok);
    ok &=
        expect("shell: interrupt_destroy(other, packet queued) = ", ours::interrupt_destroy(other),
               Status::Ok);
    ok &= empty("shell: port_wait(destroyed) = ");
    ok &=
        expect("shell: interrupt_ack(destroyed) = ", ours::interrupt_ack(other), Status::Cancelled);
    ok &= expect("shell: interrupt_bind(destroyed) = ", bind(other, OTHER_KEY), Status::Cancelled);

    // A destroy after port_wait took the packet, before its ack.
    ok &= expect("shell: interrupt_bind(rebind) = ", bind(interrupt, KEY), Status::Ok);
    ok &= expect("shell: interrupt_ack(rebound) = ", ours::interrupt_ack(interrupt), Status::Ok);
    ok &= expect("shell: interrupt_trigger(8001) = ", trigger(interrupt, 8001), Status::Ok);
    ok &= reads("shell: port_wait(8001) = ", KEY, 8001);
    ok &= expect("shell: interrupt_destroy(packet taken) = ", ours::interrupt_destroy(interrupt),
                 Status::NotFound);
    ok &= empty("shell: port_wait(nothing left) = ");

    for (auto const handle: {interrupt, other, port, plain_port}) {
        ok &= ours::handle_close(handle) == Status::Ok;
    }

    if (ok) {
        auto _ = ort::debug_line("shell: interrupt port checks passed");
    } else {
        auto _ = ort::debug_line("shell: interrupt port checks FAILED");
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
    // A snapshot kind with something beside it. NoWrite would otherwise come
    // back writable, since every snapshot is given Write; Slice is a second
    // kind, a window where a snapshot is a copy.
    ok &= expect("shell: vmo_create_child(SnapshotFull | NoWrite) = ",
                 ours::vmo_create_child(parent,
                                        VmoChildOptions::SnapshotFull | VmoChildOptions::NoWrite, 0,
                                        PAGE, &refused),
                 Status::NotSupported);
    ok &= expect("shell: vmo_create_child(SnapshotFull | Slice) = ",
                 ours::vmo_create_child(parent,
                                        VmoChildOptions::SnapshotFull | VmoChildOptions::Slice, 0,
                                        PAGE, &refused),
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

/// vmo_get_size: how big a VMO is, as the program holding it sees it.
///
/// What runs in every image, on VMOs of its own:
///   - the handle comes first: a value naming nothing and a closed one are
///     BadHandle, a channel BadType, even beside a destination in the kernel
///     half;
///   - a created VMO is its size rounded up to a page, an empty one 0;
///   - no right is needed: a duplicate with none at all still answers,
///     although it may not read;
///   - the size is the bound vmo_read checks: the last byte below it reads,
///     the one at it is OutOfRange;
///   - a slice is its own window, rounded up to a page, not its parent; a
///     snapshot is the size it asked for, even past its parent's end;
///   - a destination the kernel cannot write is InvArgs: the kernel half,
///     nothing mapped, and the read-only vDSO.
/// What needs the BootFS runs wherever this program was handed one: its VMO is
/// exactly the size its header records, and a slice of a file whose length is
/// not a whole number of pages is that length rounded up to one -- what the
/// launcher will be handed for a program and bound its parsing by.
auto check_vmo_get_size(ours::Uandle const bootfs) -> void {
    using ours::Rights;
    using ours::Status;
    using ours::VmoChildOptions;
    using ours::VmoOptions;

    constexpr auto PAGE = u64{4096};

    // One call, and the size it gave against the one wanted, on one line.
    auto const has_size = [](std::string_view const what, ours::Uandle const vmo,
                             u64 const want) -> bool {
        auto size = ~u64{0};
        auto const status = ours::vmo_get_size(vmo, &size);
        char got_text[18];
        char want_text[18];
        if (status != Status::Ok) {
            auto _ = ort::debug_line({"shell: vmo_get_size(", what, ") = ",
                                      std::string_view{ours::to_string(status)}, " (expected Ok)"});
            return false;
        }
        if (size != want) {
            auto _ =
                ort::debug_line({"shell: vmo_get_size(", what, ") = ", ort::hex(size, got_text),
                                 " (expected ", ort::hex(want, want_text), ")"});
            return false;
        }
        auto _ = ort::debug_line({"shell: vmo_get_size(", what, ") = ", ort::hex(size, got_text)});
        return true;
    };

    auto ok = true;
    u64 size = 0;

    // The handle.
    ok &= expect("shell: vmo_get_size(Uandle::Invalid) = ",
                 ours::vmo_get_size(ours::Uandle::Invalid, &size), Status::BadHandle);
    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (expect("shell: channel_create = ",
               ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        ok &= expect("shell: vmo_get_size(a channel) = ", ours::vmo_get_size(a, &size),
                     Status::BadType);
        ok &= expect("shell: vmo_get_size(a channel, into the kernel half) = ",
                     ours::vmo_get_size(a, reinterpret_cast<u64 *>(KERNEL)), Status::BadType);
        ok &= expect("shell: close(b) = ", ours::handle_close(b), Status::Ok);
        ok &= expect("shell: vmo_get_size(closed) = ", ours::vmo_get_size(b, &size),
                     Status::BadHandle);
        ok &= expect("shell: close(a) = ", ours::handle_close(a), Status::Ok);
    } else {
        ok = false;
    }

    // Created VMOs: rounded up to a page.
    ours::Uandle empty = ours::Uandle::Invalid;
    if (expect("shell: vmo_create(0 bytes) = ", ours::vmo_create(0, VmoOptions::None, &empty),
               Status::Ok)) {
        ok &= has_size("an empty VMO", empty, 0);
        ok &= expect("shell: close(empty) = ", ours::handle_close(empty), Status::Ok);
    } else {
        ok = false;
    }
    ours::Uandle vmo = ours::Uandle::Invalid;
    if (!expect("shell: vmo_create(3 pages + 1 byte) = ",
                ours::vmo_create(3 * PAGE + 1, VmoOptions::None, &vmo), Status::Ok)) {
        auto _ = ort::debug_line("shell: vmo_get_size checks FAILED");
        return;
    }
    ok &= has_size("3 pages + 1 byte", vmo, 4 * PAGE);

    // No right needed, though reading still needs Read.
    ours::Uandle bare = ours::Uandle::Invalid;
    if (expect("shell: handle_duplicate(vmo, no rights) = ",
               ours::handle_duplicate(vmo, Rights::None, &bare), Status::Ok)) {
        ok &= has_size("a handle with no rights", bare, 4 * PAGE);
        u8 byte = 0;
        ok &=
            expect("shell: vmo_read(a handle with no rights) = ", ours::vmo_read(bare, &byte, 0, 1),
                   Status::AccessDenied);
        ok &= expect("shell: close(bare) = ", ours::handle_close(bare), Status::Ok);
    } else {
        ok = false;
    }

    // The size is where vmo_read stops.
    u8 byte = 0;
    ok &= expect("shell: vmo_read(the byte before the size) = ",
                 ours::vmo_read(vmo, &byte, 4 * PAGE - 1, 1), Status::Ok);
    ok &=
        expect("shell: vmo_read(the byte at the size) = ", ours::vmo_read(vmo, &byte, 4 * PAGE, 1),
               Status::OutOfRange);

    // Children: a slice is its window, a snapshot what it asked for.
    ours::Uandle slice = ours::Uandle::Invalid;
    if (expect("shell: vmo_create_child(Slice, page 2, 1 page + 1 byte) = ",
               ours::vmo_create_child(vmo, VmoChildOptions::Slice, PAGE, PAGE + 1, &slice),
               Status::Ok)) {
        ok &= has_size("a slice of 1 page + 1 byte", slice, 2 * PAGE);
        ok &= expect("shell: close(slice) = ", ours::handle_close(slice), Status::Ok);
    } else {
        ok = false;
    }
    ours::Uandle snapshot = ours::Uandle::Invalid;
    if (expect("shell: vmo_create_child(SnapshotFull, 6 pages of a 4-page VMO) = ",
               ours::vmo_create_child(vmo, VmoChildOptions::SnapshotFull, 0, 6 * PAGE, &snapshot),
               Status::Ok)) {
        ok &= has_size("a 6-page snapshot", snapshot, 6 * PAGE);
        ok &= expect("shell: close(snapshot) = ", ours::handle_close(snapshot), Status::Ok);
    } else {
        ok = false;
    }

    // Destinations the kernel cannot write.
    ok &= expect("shell: vmo_get_size(into the kernel half) = ",
                 ours::vmo_get_size(vmo, reinterpret_cast<u64 *>(KERNEL)), Status::InvArgs);
    ok &= expect("shell: vmo_get_size(into unmapped) = ",
                 ours::vmo_get_size(vmo, reinterpret_cast<u64 *>(UNMAPPED)), Status::InvArgs);
    ok &=
        expect("shell: vmo_get_size(into the vDSO) = ",
               ours::vmo_get_size(vmo, reinterpret_cast<u64 *>(ort::vdso_base())), Status::InvArgs);
    ok &= expect("shell: close(vmo) = ", ours::handle_close(vmo), Status::Ok);

    if (bootfs == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vmo_get_size on the BootFS skipped");
    } else {
        // The whole BootFS, then one file of it whose length is not whole
        // pages, if there is one; any file otherwise, with nothing rounded.
        bootfs::Header header{};
        constexpr u32 MAX_ENTRIES = 16;
        bootfs::Entry entries[MAX_ENTRIES];
        // The entries are read only once the header has said how many there
        // are, and that they fit.
        auto const read =
            expect("shell: vmo_read(header) = ", ours::vmo_read(bootfs, &header, 0, sizeof(header)),
                   Status::Ok) &&
            header.file_count > 0 && header.file_count <= MAX_ENTRIES &&
            expect("shell: vmo_read(entries) = ",
                   ours::vmo_read(bootfs, entries, sizeof(header),
                                  usize{header.file_count} * sizeof(bootfs::Entry)),
                   Status::Ok);
        if (read) {
            ok &= has_size("bootfs", bootfs, header.total_size);
            auto const *file = &entries[0];
            for (u32 i = 0; i < header.file_count; ++i) {
                if (entries[i].data_size % PAGE != 0) {
                    file = &entries[i];
                    break;
                }
            }
            auto const rounded = (file->data_size + PAGE - 1) / PAGE * PAGE;
            ours::Uandle program = ours::Uandle::Invalid;
            if (expect("shell: vmo_create_child(bootfs, Slice, a file) = ",
                       ours::vmo_create_child(bootfs, VmoChildOptions::Slice, file->data_offset,
                                              file->data_size, &program),
                       Status::Ok)) {
                ok &= has_size("a slice of a BootFS file", program, rounded);
                ok &= expect("shell: close(the file slice) = ", ours::handle_close(program),
                             Status::Ok);
            } else {
                ok = false;
            }
        } else {
            auto _ = ort::debug_line("shell: the BootFS directory could not be read");
            ok = false;
        }
    }

    if (ok) {
        auto _ = ort::debug_line("shell: vmo_get_size checks passed");
    } else {
        auto _ = ort::debug_line("shell: vmo_get_size checks FAILED");
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

/// The resources userboot hands the program it starts, which on dev.dbg_1 is
/// this shell: the roots of the interrupt lines and of the I/O ports, found
/// by their handle types, and what object_get_info's Resource topic says of
/// each -- its kind, base 0 and size 0, which make it a root, and the name
/// the kernel gave it. vmex answers the same topic as the System slot it is.
/// Then the topic's refusals: an object that is not a resource, and a handle
/// without Inspect.
///
/// On dev, where the root started this shell as a component, the check is
/// that neither root came with it: the root keeps both for the drivers it
/// routes them to, and a component reaches them only through such a route,
/// never as a startup handle, as it never holds VMEX
/// (check_launcher_service()). Where userboot started it, both must be here:
/// the kernel mints them on every boot, so a missing one fails the check
/// rather than skipping it.
auto check_resources(ours::Uandle const irq, ours::Uandle const ioport, ours::Uandle const vmex,
                     bool const component) -> void {
    using ours::InfoResource;
    using ours::InfoTopic;
    using ours::RsrcKind;
    using ours::Status;

    if (component) {
        if (irq == ours::Uandle::Invalid && ioport == ours::Uandle::Invalid) {
            auto _ = ort::debug_line("shell: resource checks passed (the root kept both roots)");
        } else {
            auto _ = ort::debug_line("shell: holds the IRQ or I/O port root the root keeps");
            auto _ = ort::debug_line("shell: resource checks FAILED");
        }
        return;
    }

    // Whether |handle| reports itself as the resource described, saying what
    // it reported when it does not.
    auto const reports = [](std::string_view const what, ours::Uandle const handle,
                            RsrcKind const kind, u64 const base, u64 const size,
                            std::string_view const name) {
        InfoResource info{};
        usize actual = 0;
        usize avail = 0;
        if (!expect(what,
                    ours::object_get_info(handle, InfoTopic::Resource, &info, sizeof(info), &actual,
                                          &avail),
                    Status::Ok)) {
            return false;
        }
        auto const *const end = std::find(std::begin(info.name), std::end(info.name), '\0');
        auto const reported = std::string_view(info.name, end);
        if (info.kind == kind && info.base == base && info.size == size && reported == name &&
            actual == 1 && avail == 1) {
            return true;
        }
        char kind_text[18];
        char base_text[18];
        char size_text[18];
        auto _ = ort::debug_line({"shell: ", name, " reported kind ",
                                  ort::hex(static_cast<u64>(info.kind), kind_text), ", base ",
                                  ort::hex(info.base, base_text), ", size ",
                                  ort::hex(info.size, size_text), ", name ", reported});
        return false;
    };

    auto ok = reports("shell: object_get_info(the IRQ root, Resource) = ", irq, RsrcKind::Irq, 0, 0,
                      "irq");
    ok &= reports("shell: object_get_info(the I/O port root, Resource) = ", ioport,
                  RsrcKind::IoPort, 0, 0, "ioport");
    if (vmex != ours::Uandle::Invalid) {
        ok &= reports("shell: object_get_info(vmex, Resource) = ", vmex, RsrcKind::System,
                      ours::RSRC_SYSTEM_VMEX_BASE, 1, "vmex");
    }

    InfoResource info{};
    ours::Uandle port = ours::Uandle::Invalid;
    if (expect("shell: port_create = ", ours::port_create(ours::PortCreateOptions::None, &port),
               Status::Ok)) {
        ok &= expect(
            "shell: object_get_info(a port, Resource) = ",
            ours::object_get_info(port, InfoTopic::Resource, &info, sizeof(info), nullptr, nullptr),
            Status::BadType);
        ok &= expect("shell: close(the port) = ", ours::handle_close(port), Status::Ok);
    } else {
        ok = false;
    }

    ours::Uandle blind = ours::Uandle::Invalid;
    constexpr auto WITHOUT_INSPECT = ours::Rights::Transfer | ours::Rights::Duplicate;
    if (expect("shell: handle_duplicate(the IRQ root, without Inspect) = ",
               ours::handle_duplicate(irq, WITHOUT_INSPECT, &blind), Status::Ok)) {
        ok &= expect("shell: object_get_info(without Inspect) = ",
                     ours::object_get_info(blind, InfoTopic::Resource, &info, sizeof(info), nullptr,
                                           nullptr),
                     Status::AccessDenied);
        ok &= expect("shell: close(the duplicate) = ", ours::handle_close(blind), Status::Ok);
    } else {
        ok = false;
    }

    if (ok) {
        auto _ = ort::debug_line("shell: resource checks passed");
    } else {
        auto _ = ort::debug_line("shell: resource checks FAILED");
    }
}

/// Reads register |reg| of the RTC: the register's number into the index
/// port 0x70, then the register itself through the data port 0x71. Bit 7 of
/// the number masks NMIs on some chipsets, so it stays clear. Only once both
/// ports are open to this process: before that, the first out is a #GP that
/// kills it.
auto rtc_read(u8 const reg) -> u8 {
    auto value = reg;
    __asm__ volatile("outb %%al, $0x70\n\t"
                     "inb $0x71, %%al"
                     : "+a"(value));
    return value;
}

/// ioports_request and ioports_release (.agents/tasks/microkernel-interrupts/
/// PLAN.md, step 10), with the I/O port root userboot hands this shell on
/// dev.dbg_1:
///   - the refusals, before anything is open: no handle is BadHandle, a range
///     running past the last port OutOfRange, COM1, a port the kernel
///     drives, AccessDenied, and the IRQ root, a resource of another kind,
///     BadType;
///   - the RTC's two ports opened, and the RTC read through them: register
///     D's VRT bit, which says its time is valid, and a seconds register
///     that holds a second in the format register B names;
///   - while they are open here, a fault_probe that reads 0x70 is killed
///     with a #GP: a process that did not ask for a port does not have it,
///     whoever else did;
///   - the ports released again.
///
/// The probe runs on whichever CPU the scheduler places it on, which need
/// not be one this shell ran on with the ports open, so it does not show that
/// a switch closes them: ioport.test checks that, on one CPU, from the kernel.
/// Booted with a single CPU (SMP=1), the probe shares this shell's, and then
/// it does.
///
/// The RTC because the kernel drives none of its ports, so a program reading
/// it disturbs nothing, and because what it answers can be checked.
///
/// On dev, where the root started this shell as a component, the root keeps
/// the I/O port root (check_resources()), and the refusal of no handle is all
/// there is to check. Where userboot started it, the root, the job and the
/// launcher must all be here, so a missing one fails the check rather than
/// skipping it.
auto check_ioports(ours::Uandle const job, ours::Uandle const ioport, ours::Uandle const irq,
                   shell::LaunchProgram const launcher, void *const launcher_context,
                   bool const component) -> void {
    using ours::Status;

    auto ok = expect("shell: ioports_request(no handle, 0x70, 2) = ",
                     ours::ioports_request(ours::Uandle::Invalid, 0x70, 2), Status::BadHandle);
    if (component) {
        if (ok) {
            auto _ =
                ort::debug_line("shell: ioport checks passed (the root keeps the I/O port root)");
        } else {
            auto _ = ort::debug_line("shell: ioport checks FAILED");
        }
        return;
    }
    if (ioport == ours::Uandle::Invalid || job == ours::Uandle::Invalid || launcher == nullptr) {
        auto _ = ort::debug_line("shell: no I/O port root, job or launcher from userboot");
        auto _ = ort::debug_line("shell: ioport checks FAILED");
        return;
    }

    ok &= expect("shell: ioports_request(the root, 0xFFFF, 2) = ",
                 ours::ioports_request(ioport, 0xFFFF, 2), Status::OutOfRange);
    ok &= expect("shell: ioports_request(the root, COM1 0x3F8, 1) = ",
                 ours::ioports_request(ioport, 0x3F8, 1), Status::AccessDenied);
    ok &= expect("shell: ioports_request(the IRQ root, 0x70, 2) = ",
                 ours::ioports_request(irq, 0x70, 2), Status::BadType);
    ok &= expect("shell: ioports_release(the root, COM1 0x3F8, 1) = ",
                 ours::ioports_release(ioport, 0x3F8, 1), Status::AccessDenied);

    // Nothing touches 0x70 or 0x71 unless the request said they are open:
    // the first access would otherwise kill this shell.
    if (!expect("shell: ioports_request(the root, RTC 0x70, 2) = ",
                ours::ioports_request(ioport, 0x70, 2), Status::Ok)) {
        auto _ = ort::debug_line("shell: ioport checks FAILED");
        return;
    }

    auto const register_d = rtc_read(0x0D);
    auto const register_b = rtc_read(0x0B);
    auto const seconds = rtc_read(0x00);
    // Register B's bit 2 says binary; clear, the time registers are BCD.
    auto seconds_valid = false;
    if ((register_b & 0x04) != 0) {
        seconds_valid = seconds < 60;
    } else {
        seconds_valid = (seconds & 0x0F) < 10 && (seconds >> 4) < 6;
    }
    char d_text[18];
    char b_text[18];
    char seconds_text[18];
    auto _ = ort::debug_line(
        {"shell: RTC through 0x70/0x71: register D ", ort::hex(register_d, d_text), ", register B ",
         ort::hex(register_b, b_text), ", seconds ", ort::hex(seconds, seconds_text)});
    if ((register_d & 0x80) == 0 || !seconds_valid) {
        auto _ = ort::debug_line("shell: the RTC answered with an invalid time");
        ok = false;
    }

    // The probe needs no resource to be killed: it asks for nothing.
    auto const probe = run_probe(job, "gp_io_rtc", false, launcher, launcher_context);
    if (probe.started) {
        auto const code = return_code_of(probe.process);
        auto _ = ours::handle_close(probe.process);
        auto const killed = code.is_ok() && code.unwrap() == ours::TASK_RETCODE_EXCEPTION_KILL;
        auto const general = probe.exception_channel_ok && probe.exceptions_seen == 1 &&
                             probe.exception_type == ours::ExceptionType::General;
        char type_text[18];
        auto const type = ort::hex(static_cast<u64>(probe.exception_type), type_text);
        if (killed && general) {
            auto _ = ort::debug_line({"shell: probe gp_io_rtc killed after exception ", type,
                                      ", while 0x70 is open in this shell"});
        } else {
            auto _ = ort::debug_line({"shell: probe gp_io_rtc was not killed by a #GP (exception ",
                                      type, ") while 0x70 is open in this shell"});
            ok = false;
        }
    } else {
        ok = false;
    }

    ok &= expect("shell: ioports_release(the root, RTC 0x70, 2) = ",
                 ours::ioports_release(ioport, 0x70, 2), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: ioport checks passed");
    } else {
        auto _ = ort::debug_line("shell: ioport checks FAILED");
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
    ours::VirtAddr at{};
    ok &= expect("shell: vma_map(R, vmo_offset = page 2) = ",
                 ours::vma_map(vma, R, 0, vmo, PAGE, PAGE, &at), Status::Ok);
    if (at != ours::VirtAddr{0}) {
        auto const *const bytes = reinterpret_cast<u8 const volatile *>(at);
        u8 got[4] = {bytes[0], bytes[1], bytes[2], bytes[3]};
        ok &= same_bytes(second, got, sizeof(got));
    }

    // Read-write: a store through the mapping is what vmo_read finds.
    ours::VirtAddr rw{};
    ok &= expect("shell: vma_map(RW) = ", ours::vma_map(vma, RW, 0, vmo, 0, PAGE, &rw), Status::Ok);
    if (rw != ours::VirtAddr{0}) {
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
    ours::VirtAddr sliced{};
    ok &=
        expect("shell: vma_map(R, the slice) = ", ours::vma_map(vma, R, 0, slice, 0, PAGE, &sliced),
               Status::Ok);
    if (sliced != ours::VirtAddr{0}) {
        ok &= *reinterpret_cast<u8 const volatile *>(sliced) == second[0];
    }
    ok &= expect("shell: vma_map(R, 2 pages of a 1-page slice) = ",
                 ours::vma_map(vma, R, 0, slice, 0, 2 * PAGE, &sliced), Status::OutOfRange);
    ok &= expect("shell: close(slice) = ", ours::handle_close(slice), Status::Ok);

    // The protection.
    ours::VirtAddr refused{};
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
        ours::VirtAddr code{};
        ok &= expect("shell: vma_map(RX, the executable BootFS) = ",
                     ours::vma_map(vma, RX, 0, executable, 0, PAGE, &code), Status::Ok);
        if (code != ours::VirtAddr{0}) {
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
        ours::VirtAddr code{};
        ok &= expect("shell: vma_map(RX, the executable VMO) = ",
                     ours::vma_map(vma, RX, 0, executable, 0, PAGE, &code), Status::Ok);
        ok &= expect("shell: vmo_create_child(the executable VMO, SnapshotFull) = ",
                     ours::vmo_create_child(executable, ours::VmoChildOptions::SnapshotFull, 0,
                                            PAGE, &snapshot),
                     Status::Ok);
        ok &= expect("shell: vma_map(RX, its snapshot) = ",
                     ours::vma_map(vma, RX, 0, snapshot, 0, PAGE, &refused), Status::AccessDenied);
        ours::VirtAddr data{};
        ok &= expect("shell: vma_map(RW, its snapshot) = ",
                     ours::vma_map(vma, RW, 0, snapshot, 0, PAGE, &data), Status::Ok);
        if (data != ours::VirtAddr{0} && code != ours::VirtAddr{0}) {
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
///   - a value the kernel cannot write back, the address or the handle, and
///     a handle it cannot make, this program being at its handle quota,
///     release the range: the same spot can be reserved on the next call.
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
    ours::VirtAddr at{};
    ok &= expect("shell: vma_allocate(Specific, 1 page) = ",
                 ours::vma_allocate(vma, VmOptions::Specific, SPOT, PAGE, &guard, &at), Status::Ok);
    // Where the root region starts, so that an address the kernel chose can be
    // turned back into an offset for a Specific request.
    auto const root_base = at - SPOT;
    ours::VirtAddr mapped{};
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
    ours::VirtAddr guard_at{};
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
    ours::VirtAddr region_at{};
    ok &= expect(
        "shell: vma_allocate(CanMapRead | CanMapWrite | CanMapSpecific, anywhere, 2 pages) = ",
        ours::vma_allocate(vma, CAN_MAP_RW_SPECIFIC, 0, 2 * PAGE, &region, &region_at), Status::Ok);
    if (region_at == ours::VirtAddr{0} || std::to_underlying(region_at) % PAGE != 0) {
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
    ours::VirtAddr nested_at{};
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
    ours::VirtAddr read_only_at{};
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
    ours::VirtAddr outer_at{};
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
    ours::VirtAddr plain_at{};
    ok &= expect("shell: vma_allocate(CanMapRead, anywhere, 2 pages) = ",
                 ours::vma_allocate(vma, VmOptions::CanMapRead, 0, 2 * PAGE, &plain, &plain_at),
                 Status::Ok);
    ok &= expect("shell: vma_map(R, Specific, into a sub-region without CanMapSpecific) = ",
                 ours::vma_map(plain, SPECIFIC_R, 0, vmo, 0, PAGE, &mapped), Status::AccessDenied);
    // The flag is asked before the range: a place past the end of a region
    // without it is AccessDenied, not InvArgs.
    ok &= expect("shell: vma_map(R, Specific, past the end of a sub-region without it) = ",
                 ours::vma_map(plain, SPECIFIC_R, 2 * PAGE, vmo, 0, PAGE, &mapped),
                 Status::AccessDenied);
    ours::Uandle inner = ours::Uandle::Invalid;
    ours::VirtAddr inner_at{};
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
    constexpr auto UNWRITTEN = SPOT + 24 * PAGE;
    constexpr auto UNHANDLED = SPOT + 28 * PAGE;
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
    // A child the kernel cannot write: the handle never reaches the table, and
    // the range goes back the same way.
    ok &= expect("shell: vma_allocate(child = kernel half) = ",
                 ours::vma_allocate(vma, VmOptions::Specific, UNWRITTEN, PAGE,
                                    reinterpret_cast<ours::Uandle *>(KERNEL), &at),
                 Status::InvArgs);
    ok &= expect("shell: vma_allocate(that spot again) = ",
                 ours::vma_allocate(vma, VmOptions::Specific, UNWRITTEN, PAGE, &guard, &at),
                 Status::Ok);
    ok &= expect("shell: close(guard) = ", ours::handle_close(guard), Status::Ok);
    // A handle the kernel cannot make: with the table filled to this
    // program's quota, as check_handle_quota() fills it, the sub-region is
    // made and then its handle refused, which vma_allocate answers with
    // NoResource. The range goes back as it does for a value that cannot be
    // written, so with one handle closed the same spot is taken at once.
    constexpr u32 ROOM = ours::HANDLE_MAX_PER_PROCESS + 2;
    static ours::Uandle FILLER[ROOM];
    u32 held = 0;
    while (held + 2 <= ROOM && ours::channel_create(ours::ChannelCreateOptions::None, &FILLER[held],
                                                    &FILLER[held + 1]) == Status::Ok) {
        held += 2;
    }
    // A pair needs two slots, so at most one is left; fill it too.
    if (held < ROOM &&
        ours::handle_duplicate(vmo, ours::Rights::SameRights, &FILLER[held]) == Status::Ok) {
        held += 1;
    }
    ok &= expect("shell: vma_allocate(at the handle quota) = ",
                 ours::vma_allocate(vma, VmOptions::Specific, UNHANDLED, PAGE, &guard, &at),
                 Status::NoResource);
    if (held != 0) {
        held -= 1;
        ok &= ours::handle_close(FILLER[held]) == Status::Ok;
    }
    ok &= expect("shell: vma_allocate(that spot, a handle closed) = ",
                 ours::vma_allocate(vma, VmOptions::Specific, UNHANDLED, PAGE, &guard, &at),
                 Status::Ok);
    ok &= expect("shell: close(guard) = ", ours::handle_close(guard), Status::Ok);
    for (u32 i = 0; i < held; ++i) {
        ok &= ours::handle_close(FILLER[i]) == Status::Ok;
    }
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
    static ours::Uandle FILLER[ROOM];
    u32 held = 0;
    while (held + 2 <= ROOM && ours::channel_create(ours::ChannelCreateOptions::None, &FILLER[held],
                                                    &FILLER[held + 1]) == Status::Ok) {
        held += 2;
    }
    // A pair needs two slots, so at most one is left; fill it too.
    if (held < ROOM &&
        ours::handle_duplicate(job, Rights::SameRights, &FILLER[held]) == Status::Ok) {
        held += 1;
    }
    ok &= held >= 2;
    if (held >= 2) {
        ok &= ours::handle_close(FILLER[held - 1]) == Status::Ok;
        ok &= ours::handle_close(FILLER[held - 2]) == Status::Ok;
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
        ok &= ours::handle_close(FILLER[i]) == Status::Ok;
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
    static ours::Uandle FILLER[ROOM];
    u32 held = 0;
    while (held + 2 <= ROOM && ours::channel_create(ours::ChannelCreateOptions::None, &FILLER[held],
                                                    &FILLER[held + 1]) == Status::Ok) {
        held += 2;
    }
    if (held < ROOM &&
        ours::handle_duplicate(job, Rights::SameRights, &FILLER[held]) == Status::Ok) {
        held += 1;
    }
    ok &= held >= 2;
    if (held >= 2) {
        ok &= ours::handle_close(FILLER[held - 1]) == Status::Ok;
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
        ok &= ours::handle_close(FILLER[i]) == Status::Ok;
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

    // A thread that has not started raises nothing: not RUNNING, and not
    // TERMINATED either, which it raises only once it has run and left.
    {
        constexpr ours::Instant NOW = 0;
        ok &= expect("shell: wait_one(a thread not started, Running|Terminated) = ",
                     ours::object_wait_one(thread, Signals::ThreadRunning | Signals::Terminated,
                                           NOW, nullptr),
                     Status::TimeOut);
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

/// The second thread of check_thread_start(): it writes the two words it was
/// started with -- the channel end it was handed and a value -- to that
/// channel end, and ends. Nothing but system calls: this program's runtime is
/// not written for a second thread, and the thread uses none of it.
[[noreturn]]
auto second_thread(usize const channel, usize const value) -> void {
    usize const words[2] = {channel, value};
    auto _ = ours::channel_write(static_cast<ours::Uandle>(static_cast<u32>(channel)),
                                 ours::ChannelWriteOptions::None, words, sizeof(words), nullptr, 0);
    ours::thread_exit();
}

/// thread_start and thread_exit (step 6 of .agents/tasks/task-control/): a
/// second thread of this very process, on a stack mapped here, that reports
/// back on a channel and ends.
///
///   - the handle: none is BadHandle, a channel BadType, a thread handle
///     without ManageThread AccessDenied; an entry outside user space is
///     InvArgs; each refusal leaves the thread to be started;
///   - the start: the thread runs at its entry with both arguments, and once
///     it has called thread_exit it reads Terminated and no longer Running;
///   - a second start at once, while the thread has most likely not run yet,
///     is BadState and leaves the first one's thread alone
///     (docs/changes/261023/object.md), and a start of the ended thread is
///     BadState too.
///
/// The waits are infinite: object_wait_one takes only 0 or INSTANT_INFINITE,
/// so a thread that never runs or never ends hangs the shell here rather than
/// being reported.
auto check_thread_start(ours::Uandle const vma) -> void {
    using ours::Rights;
    using ours::Signals;
    using ours::Status;

    constexpr auto STACK_SIZE = u64{4 * 4096};
    constexpr usize VALUE = 0x5ec0'7d7e'ad00'0006;
    constexpr auto NOT_USER = ours::VirtAddr{0xffff'8000'0000'0000};

    if (vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: thread_start checks skipped (no VmaRoot)");
        return;
    }

    auto thread = ours::Uandle::Invalid;
    auto stack = ours::Uandle::Invalid;
    auto here = ours::Uandle::Invalid;
    auto there = ours::Uandle::Invalid;
    ours::VirtAddr stack_at{};
    if (!expect("shell: thread_create(this process) = ",
                ours::thread_create(ort::process_self(), "second", sizeof("second") - 1,
                                    ours::ThreadCreateOptions::None, &thread),
                Status::Ok) ||
        !expect("shell: vmo_create(the second thread's stack) = ",
                ours::vmo_create(STACK_SIZE, ours::VmoOptions::None, &stack), Status::Ok) ||
        !expect("shell: vma_map(the second thread's stack) = ",
                ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite, 0, stack,
                              0, STACK_SIZE, &stack_at),
                Status::Ok) ||
        !expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &here, &there),
                Status::Ok)) {
        for (auto const handle: {thread, stack, here, there}) {
            if (handle != ours::Uandle::Invalid) {
                auto _ = ours::handle_close(handle);
            }
        }
        if (stack_at != ours::VirtAddr{0}) {
            auto _ = ours::vma_unmap(vma, stack_at, STACK_SIZE);
        }
        auto _ = ort::debug_line("shell: thread_start checks FAILED");
        return;
    }

    auto ok = true;
    // The mapping holds the VMO.
    ok &= expect("shell: close(the stack's VMO) = ", ours::handle_close(stack), Status::Ok);

    auto const entry = ours::VirtAddr{reinterpret_cast<usize>(&second_thread)};
    // As a launcher sets a first thread's: 16-byte aligned, less the return
    // address a call would have pushed (SysV).
    auto const sp = ours::VirtAddr{std::to_underlying(stack_at + STACK_SIZE) & ~15_usize} - 8;
    auto const channel = static_cast<usize>(static_cast<u32>(there));

    // The handle, and the entry.
    ok &= expect("shell: thread_start(Uandle::Invalid) = ",
                 ours::thread_start(ours::Uandle::Invalid, entry, sp, channel, VALUE),
                 Status::BadHandle);
    ok &= expect("shell: thread_start(a channel) = ",
                 ours::thread_start(here, entry, sp, channel, VALUE), Status::BadType);
    auto weak = ours::Uandle::Invalid;
    if (expect("shell: handle_duplicate(the thread, without ManageThread) = ",
               ours::handle_duplicate(thread, Rights::Basic, &weak), Status::Ok)) {
        ok &= expect("shell: thread_start(without ManageThread) = ",
                     ours::thread_start(weak, entry, sp, channel, VALUE), Status::AccessDenied);
        ok &= expect("shell: close(that handle) = ", ours::handle_close(weak), Status::Ok);
    } else {
        ok = false;
    }
    ok &= expect("shell: thread_start(an entry outside user space) = ",
                 ours::thread_start(thread, NOT_USER, sp, channel, VALUE), Status::InvArgs);

    // The start, and a second one at once.
    ok &= expect("shell: thread_start = ", ours::thread_start(thread, entry, sp, channel, VALUE),
                 Status::Ok);
    ok &= expect("shell: thread_start(again, at once) = ",
                 ours::thread_start(thread, entry, sp, channel, VALUE), Status::BadState);

    // What the thread reports.
    auto seen = Signals::None;
    ok &= expect("shell: wait_one(the second thread's channel, Readable) = ",
                 ours::object_wait_one(here, Signals::Readable, ours::INSTANT_INFINITE, &seen),
                 Status::Ok);
    usize words[2] = {};
    u32 bytes = 0;
    ok &= expect("shell: channel_read(the second thread's words) = ",
                 ours::channel_read(here, ours::ChannelReadOptions::None, words, nullptr,
                                    sizeof(words), 0, &bytes, nullptr),
                 Status::Ok);
    if (bytes != sizeof(words) || words[0] != channel || words[1] != VALUE) {
        auto _ = ort::debug_line("shell: the second thread did not report both of its arguments");
        ok = false;
    }

    // Its end.
    seen = Signals::None;
    ok &= expect("shell: wait_one(the second thread, Terminated) = ",
                 ours::object_wait_one(thread, Signals::Terminated, ours::INSTANT_INFINITE, &seen),
                 Status::Ok);
    if ((seen & Signals::ThreadRunning) != Signals::None) {
        auto _ = ort::debug_line("shell: the ended thread still reads Running");
        ok = false;
    }
    ok &= expect("shell: thread_start(the ended thread) = ",
                 ours::thread_start(thread, entry, sp, channel, VALUE), Status::BadState);

    ok &= expect("shell: close(the thread) = ", ours::handle_close(thread), Status::Ok);
    ok &= expect("shell: close(here) = ", ours::handle_close(here), Status::Ok);
    ok &= expect("shell: close(there) = ", ours::handle_close(there), Status::Ok);
    ok &= expect("shell: vma_unmap(the second thread's stack) = ",
                 ours::vma_unmap(vma, stack_at, STACK_SIZE), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: thread_start checks passed");
    } else {
        auto _ = ort::debug_line("shell: thread_start checks FAILED");
    }
}

/// A second thread of this process for the suspend checks, and the stack it
/// runs on.
struct SecondThread {
    ours::Uandle thread = ours::Uandle::Invalid;
    ours::VirtAddr stack_at{};
};

constexpr auto SECOND_STACK_SIZE = u64{4 * 4096};

/// Makes a thread of this process, not started, and maps a stack for it.
/// False, reported, when either cannot be made; nothing is left behind then.
auto make_second_thread(ours::Uandle const vma, SecondThread *const out) -> bool {
    using ours::Status;

    if (!expect("shell: thread_create(this process) = ",
                ours::thread_create(ort::process_self(), "second", sizeof("second") - 1,
                                    ours::ThreadCreateOptions::None, &out->thread),
                Status::Ok)) {
        return false;
    }
    auto stack = ours::Uandle::Invalid;
    if (!expect("shell: vmo_create(a second thread's stack) = ",
                ours::vmo_create(SECOND_STACK_SIZE, ours::VmoOptions::None, &stack), Status::Ok)) {
        auto _ = ours::handle_close(out->thread);
        return false;
    }
    auto const mapped =
        expect("shell: vma_map(a second thread's stack) = ",
               ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite, 0, stack,
                             0, SECOND_STACK_SIZE, &out->stack_at),
               Status::Ok);
    // The mapping holds the VMO.
    auto _ = ours::handle_close(stack);
    if (!mapped) {
        auto _ = ours::handle_close(out->thread);
        return false;
    }
    return true;
}

/// Starts |second| at |entry| with |arg1| and |arg2|, its stack pointer set
/// as a launcher sets a first thread's: 16-byte aligned, less the return
/// address a call would have pushed (SysV).
auto start_second_thread(SecondThread const &second, void (*const entry)(usize, usize),
                         usize const arg1, usize const arg2) -> ours::Status {
    auto const sp =
        ours::VirtAddr{std::to_underlying(second.stack_at + SECOND_STACK_SIZE) & ~15_usize} - 8;
    return ours::thread_start(second.thread, ours::VirtAddr{reinterpret_cast<usize>(entry)}, sp,
                              arg1, arg2);
}

/// Closes an ended |second| and unmaps its stack.
auto drop_second_thread(ours::Uandle const vma, SecondThread const &second) -> bool {
    using ours::Status;

    auto ok =
        expect("shell: close(a second thread) = ", ours::handle_close(second.thread), Status::Ok);
    ok &= expect("shell: vma_unmap(a second thread's stack) = ",
                 ours::vma_unmap(vma, second.stack_at, SECOND_STACK_SIZE), Status::Ok);
    return ok;
}

/// Spends time on this thread without a system call -- there is none to sleep
/// with -- so that another thread runs meanwhile, on another CPU or once this
/// one's time slice is up.
auto spin(u64 const rounds) -> void {
    for (u64 round = 0; round < rounds; ++round) {
        __builtin_ia32_pause();
    }
}

/// How long the suspend checks let a thread run, or watch one that should not:
/// long against a context switch under QEMU's TCG.
constexpr u64 SPIN_ROUNDS = 2'000'000;

/// The handle a second thread was handed, as a value in an argument register.
auto to_uandle(usize const value) -> ours::Uandle {
    return static_cast<ours::Uandle>(static_cast<u32>(value));
}

/// What counting_thread() counts in, and the flag that ends it. Static, not on
/// the stack of the check that starts the thread: a check that gives up early
/// must not leave a thread writing into a frame that is gone.
std::atomic<u64> COUNTED{0};
std::atomic<bool> STOP_COUNTING{false};

/// A second thread that counts in |counter| until |stop| is set, making no
/// system call meanwhile: a suspend reaches it only through an interrupt.
[[noreturn]]
auto counting_thread(usize const counter, usize const stop) -> void {
    auto &count = *reinterpret_cast<std::atomic<u64> *>(counter);
    auto const &done = *reinterpret_cast<std::atomic<bool> const *>(stop);
    while (!done.load(std::memory_order_acquire)) {
        count.fetch_add(1, std::memory_order_relaxed);
    }
    ours::thread_exit();
}

/// Whether COUNTED goes past |past| within a bounded time.
auto counts_past(u64 const past) -> bool {
    for (u32 round = 0; round < 1000; ++round) {
        if (COUNTED.load(std::memory_order_relaxed) > past) {
            return true;
        }
        spin(SPIN_ROUNDS / 10);
    }
    return false;
}

/// A second thread that says it is ready on |report|, then waits on |port|,
/// and writes back what port_wait answered and the key of what it got.
[[noreturn]]
auto port_waiting_thread(usize const port, usize const report) -> void {
    u64 const ready = 1;
    auto _ = ours::channel_write(to_uandle(report), ours::ChannelWriteOptions::None, &ready,
                                 sizeof(ready), nullptr, 0);
    ours::PortPacket packet{};
    auto const status = ours::port_wait(to_uandle(port), ours::INSTANT_INFINITE, &packet);
    u64 const words[2] = {static_cast<u64>(status), packet.key};
    auto _ = ours::channel_write(to_uandle(report), ours::ChannelWriteOptions::None, words,
                                 sizeof(words), nullptr, 0);
    ours::thread_exit();
}

/// A second thread that asks to suspend itself, through the handle to it in
/// |self|, and writes back the answer.
[[noreturn]]
auto self_suspending_thread(usize const self, usize const report) -> void {
    auto token = ours::Uandle::Invalid;
    auto const status = static_cast<u64>(ours::task_suspend(to_uandle(self), &token));
    if (token != ours::Uandle::Invalid) {
        auto _ = ours::handle_close(token);
    }
    auto _ = ours::channel_write(to_uandle(report), ours::ChannelWriteOptions::None, &status,
                                 sizeof(status), nullptr, 0);
    ours::thread_exit();
}

/// One report of |count| words from a second thread on |channel|, waited for.
auto read_report(ours::Uandle const channel, u64 *const words, u32 const count) -> bool {
    using ours::Status;

    if (!expect("shell: wait_one(a second thread's report, Readable) = ",
                ours::object_wait_one(channel, ours::Signals::Readable, ours::INSTANT_INFINITE,
                                      nullptr),
                Status::Ok)) {
        return false;
    }
    u32 bytes = 0;
    auto const size = static_cast<u32>(count * sizeof(u64));
    auto const read = expect("shell: channel_read(a second thread's report) = ",
                             ours::channel_read(channel, ours::ChannelReadOptions::None, words,
                                                nullptr, size, 0, &bytes, nullptr),
                             Status::Ok);
    return read && bytes == size;
}

/// task_suspend and its token (step 7 of .agents/tasks/task-control/), on
/// second threads of this process:
///
///   - the refusals: no handle is BadHandle, a VMA BadType, a process
///     NotSupported, a thread handle without Write AccessDenied, the calling
///     thread itself NotSupported (a second thread asks, through its own
///     handle), an ended thread BadState; and a token cannot be duplicated;
///   - suspended before it is started, a thread stops before its first
///     instruction: it reads ThreadSuspended and has counted nothing; closing
///     the token lets it run;
///   - a thread counting in user mode stops: it reads ThreadSuspended and its
///     count stands still. Suspends nest: with two tokens, closing one leaves
///     it stopped, and closing the last lets it count again;
///   - a thread parked in port_wait, suspended and resumed, still gets the
///     packet queued for it afterwards, with Ok: the vDSO made the
///     interrupted call again, and InternalIrqRetry never reached the
///     thread's code.
///
/// That the suspend lands while that thread is parked, rather than just before
/// it parks, is likely and not certain: the thread says it is ready just
/// before it calls port_wait, and this waits a while before suspending it.
/// Every wait is infinite, as in check_thread_start().
auto check_task_suspend(ours::Uandle const vma) -> void {
    using ours::Rights;
    using ours::Signals;
    using ours::Status;

    constexpr auto KEY = u64{0x5005'9e4d'0000'0007};

    if (vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: task_suspend checks skipped (no VmaRoot)");
        return;
    }

    auto ok = true;
    auto token = ours::Uandle::Invalid;
    ok &= expect("shell: task_suspend(Uandle::Invalid) = ",
                 ours::task_suspend(ours::Uandle::Invalid, &token), Status::BadHandle);
    ok &= expect("shell: task_suspend(a VMA) = ", ours::task_suspend(vma, &token), Status::BadType);
    ok &= expect("shell: task_suspend(this process) = ",
                 ours::task_suspend(ort::process_self(), &token), Status::NotSupported);

    // A counting thread, suspended before it is started.
    SecondThread counter;
    if (!make_second_thread(vma, &counter)) {
        auto _ = ort::debug_line("shell: task_suspend checks FAILED");
        return;
    }
    auto weak = ours::Uandle::Invalid;
    if (expect("shell: handle_duplicate(a thread, without Write) = ",
               ours::handle_duplicate(counter.thread, Rights::Basic, &weak), Status::Ok)) {
        ok &= expect("shell: task_suspend(without Write) = ", ours::task_suspend(weak, &token),
                     Status::AccessDenied);
        ok &= expect("shell: close(that handle) = ", ours::handle_close(weak), Status::Ok);
    } else {
        ok = false;
    }

    COUNTED.store(0, std::memory_order_relaxed);
    STOP_COUNTING.store(false, std::memory_order_relaxed);
    auto first = ours::Uandle::Invalid;
    ok &= expect("shell: task_suspend(a thread not started) = ",
                 ours::task_suspend(counter.thread, &first), Status::Ok);
    auto copy = ours::Uandle::Invalid;
    ok &= expect("shell: handle_duplicate(a token) = ",
                 ours::handle_duplicate(first, Rights::SameRights, &copy), Status::AccessDenied);
    ok &= expect("shell: thread_start(a held thread) = ",
                 start_second_thread(counter, &counting_thread, reinterpret_cast<usize>(&COUNTED),
                                     reinterpret_cast<usize>(&STOP_COUNTING)),
                 Status::Ok);
    ok &= expect("shell: wait_one(a held thread, ThreadSuspended) = ",
                 ours::object_wait_one(counter.thread, Signals::ThreadSuspended,
                                       ours::INSTANT_INFINITE, nullptr),
                 Status::Ok);
    if (COUNTED.load(std::memory_order_relaxed) != 0) {
        auto _ = ort::debug_line("shell: a thread suspended before it started ran its code");
        ok = false;
    }

    // Let go of it: it counts.
    ok &= expect("shell: close(the token) = ", ours::handle_close(first), Status::Ok);
    ok &= expect("shell: wait_one(a released thread, ThreadRunning) = ",
                 ours::object_wait_one(counter.thread, Signals::ThreadRunning,
                                       ours::INSTANT_INFINITE, nullptr),
                 Status::Ok);
    if (!counts_past(0)) {
        auto _ = ort::debug_line("shell: a released thread did not count");
        ok = false;
    }

    // Suspend it while it counts, then again: two holds.
    auto second = ours::Uandle::Invalid;
    auto third = ours::Uandle::Invalid;
    ok &= expect("shell: task_suspend(a counting thread) = ",
                 ours::task_suspend(counter.thread, &second), Status::Ok);
    ok &= expect("shell: wait_one(a counting thread, ThreadSuspended) = ",
                 ours::object_wait_one(counter.thread, Signals::ThreadSuspended,
                                       ours::INSTANT_INFINITE, nullptr),
                 Status::Ok);
    auto const held_at = COUNTED.load(std::memory_order_relaxed);
    spin(SPIN_ROUNDS);
    if (COUNTED.load(std::memory_order_relaxed) != held_at) {
        auto _ = ort::debug_line("shell: a suspended thread kept counting");
        ok = false;
    }
    ok &= expect("shell: task_suspend(a suspended thread, again) = ",
                 ours::task_suspend(counter.thread, &third), Status::Ok);
    ok &=
        expect("shell: close(the first of two tokens) = ", ours::handle_close(second), Status::Ok);
    spin(SPIN_ROUNDS);
    if (COUNTED.load(std::memory_order_relaxed) != held_at) {
        auto _ = ort::debug_line("shell: a thread one token still holds ran");
        ok = false;
    }
    ok &= expect("shell: wait_one(a thread one token holds, ThreadSuspended, poll) = ",
                 ours::object_wait_one(counter.thread, Signals::ThreadSuspended, 0, nullptr),
                 Status::Ok);
    ok &= expect("shell: close(the last token) = ", ours::handle_close(third), Status::Ok);
    ok &= expect("shell: wait_one(a released thread, ThreadRunning) = ",
                 ours::object_wait_one(counter.thread, Signals::ThreadRunning,
                                       ours::INSTANT_INFINITE, nullptr),
                 Status::Ok);
    if (!counts_past(held_at)) {
        auto _ = ort::debug_line("shell: a thread released from two holds did not count again");
        ok = false;
    }

    // Its end, after which a suspend is refused.
    STOP_COUNTING.store(true, std::memory_order_release);
    ok &= expect(
        "shell: wait_one(the counting thread, Terminated) = ",
        ours::object_wait_one(counter.thread, Signals::Terminated, ours::INSTANT_INFINITE, nullptr),
        Status::Ok);
    ok &= expect("shell: task_suspend(an ended thread) = ",
                 ours::task_suspend(counter.thread, &token), Status::BadState);
    ok &= drop_second_thread(vma, counter);

    // A thread parked in port_wait, suspended and resumed: its call is made
    // again, and it gets the packet queued afterwards.
    auto port = ours::Uandle::Invalid;
    auto report = ours::Uandle::Invalid;
    auto report_there = ours::Uandle::Invalid;
    SecondThread waiter;
    if (!expect("shell: port_create = ", ours::port_create(ours::PortCreateOptions::None, &port),
                Status::Ok) ||
        !expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &report, &report_there),
                Status::Ok) ||
        !make_second_thread(vma, &waiter)) {
        auto _ = ort::debug_line("shell: task_suspend checks FAILED");
        return;
    }
    auto const report_value = static_cast<usize>(static_cast<u32>(report_there));
    ok &= expect("shell: thread_start(a port waiter) = ",
                 start_second_thread(waiter, &port_waiting_thread,
                                     static_cast<usize>(static_cast<u32>(port)), report_value),
                 Status::Ok);
    u64 ready = 0;
    ok &= read_report(report, &ready, 1) && ready == 1;
    // Into its wait, most likely.
    spin(SPIN_ROUNDS);
    auto waiting = ours::Uandle::Invalid;
    ok &= expect("shell: task_suspend(a thread in port_wait) = ",
                 ours::task_suspend(waiter.thread, &waiting), Status::Ok);
    ok &= expect("shell: wait_one(a thread taken out of port_wait, ThreadSuspended) = ",
                 ours::object_wait_one(waiter.thread, Signals::ThreadSuspended,
                                       ours::INSTANT_INFINITE, nullptr),
                 Status::Ok);
    ok &= expect("shell: close(its token) = ", ours::handle_close(waiting), Status::Ok);
    ok &= expect("shell: wait_one(the port waiter, ThreadRunning) = ",
                 ours::object_wait_one(waiter.thread, Signals::ThreadRunning,
                                       ours::INSTANT_INFINITE, nullptr),
                 Status::Ok);
    ours::PortPacket packet{};
    packet.key = KEY;
    packet.type = ours::PacketType::User;
    packet.status = Status::Ok;
    ok &= expect("shell: port_queue = ", ours::port_queue(port, &packet), Status::Ok);
    u64 got[2] = {};
    if (!read_report(report, got, 2) || got[0] != static_cast<u64>(Status::Ok) || got[1] != KEY) {
        auto _ = ort::debug_line({"shell: the resumed port_wait answered ",
                                  std::string_view{ours::to_string(static_cast<Status>(got[0]))},
                                  ", not Ok with its packet"});
        ok = false;
    }
    ok &= expect(
        "shell: wait_one(the port waiter, Terminated) = ",
        ours::object_wait_one(waiter.thread, Signals::Terminated, ours::INSTANT_INFINITE, nullptr),
        Status::Ok);
    ok &= drop_second_thread(vma, waiter);

    // A thread asking to suspend itself.
    SecondThread asker;
    if (make_second_thread(vma, &asker)) {
        ok &= expect("shell: thread_start(a thread that suspends itself) = ",
                     start_second_thread(asker, &self_suspending_thread,
                                         static_cast<usize>(static_cast<u32>(asker.thread)),
                                         report_value),
                     Status::Ok);
        u64 answer = 0;
        if (!read_report(report, &answer, 1) || answer != static_cast<u64>(Status::NotSupported)) {
            auto _ =
                ort::debug_line({"shell: task_suspend(the calling thread) = ",
                                 std::string_view{ours::to_string(static_cast<Status>(answer))},
                                 " (expected NotSupported)"});
            ok = false;
        }
        ok &= expect("shell: wait_one(the thread that asked, Terminated) = ",
                     ours::object_wait_one(asker.thread, Signals::Terminated,
                                           ours::INSTANT_INFINITE, nullptr),
                     Status::Ok);
        ok &= drop_second_thread(vma, asker);
    } else {
        ok = false;
    }

    ok &= expect("shell: close(the port) = ", ours::handle_close(port), Status::Ok);
    ok &= expect("shell: close(the report channel) = ", ours::handle_close(report), Status::Ok);
    ok &= expect("shell: close(its other end) = ", ours::handle_close(report_there), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: task_suspend checks passed");
    } else {
        auto _ = ort::debug_line("shell: task_suspend checks FAILED");
    }
}

/// Whether |process| has ended with TASK_RETCODE_SYSCALL_KILL, as task_kill
/// ends one: what object_get_info(Process) says of it once it is Terminated.
auto killed_by_task_kill(ours::Uandle const process) -> bool {
    using ours::Status;

    ours::InfoProcess info{};
    if (!expect("shell: object_get_info(a killed process, Process) = ",
                ours::object_get_info(process, ours::InfoTopic::Process, &info, sizeof(info),
                                      nullptr, nullptr),
                Status::Ok)) {
        return false;
    }
    if ((info.flags & ours::InfoProcessFlags::Exited) == ours::InfoProcessFlags{} ||
        info.return_code != ours::TASK_RETCODE_SYSCALL_KILL) {
        auto _ = ort::debug_line("shell: a killed process did not end with "
                                 "TASK_RETCODE_SYSCALL_KILL");
        return false;
    }
    return true;
}

/// task_kill (step 8 of .agents/tasks/task-control/), on both images, with
/// nothing started:
///
///   - the refusals: no handle is BadHandle, a VMA BadType, a thread
///     NotSupported, the job NotSupported (AccessDenied where this shell's
///     handle to it lacks Destroy), a process handle without Destroy
///     AccessDenied;
///   - a process not started is Dead at once: it reads Terminated as the call
///     returns, ended with TASK_RETCODE_SYSCALL_KILL, and a second kill
///     answers Ok and changes nothing.
auto check_task_kill(ours::Uandle const job, ours::Uandle const vma) -> void {
    using ours::Rights;
    using ours::Signals;
    using ours::Status;

    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: task_kill checks skipped (no root job)");
        return;
    }

    auto ok = true;
    ok &= expect("shell: task_kill(Uandle::Invalid) = ", ours::task_kill(ours::Uandle::Invalid),
                 Status::BadHandle);
    if (vma != ours::Uandle::Invalid) {
        ok &= expect("shell: task_kill(a VMA) = ", ours::task_kill(vma), Status::BadType);
    }
    auto thread = ours::Uandle::Invalid;
    if (expect("shell: thread_create(this process) = ",
               ours::thread_create(ort::process_self(), "unstarted", sizeof("unstarted") - 1,
                                   ours::ThreadCreateOptions::None, &thread),
               Status::Ok)) {
        ok &=
            expect("shell: task_kill(a thread) = ", ours::task_kill(thread), Status::NotSupported);
        ok &= expect("shell: close(the thread) = ", ours::handle_close(thread), Status::Ok);
    } else {
        ok = false;
    }
    ours::InfoHandleBasic basic{};
    if (expect("shell: object_get_info(the job, HandleBasic) = ",
               ours::object_get_info(job, ours::InfoTopic::HandleBasic, &basic, sizeof(basic),
                                     nullptr, nullptr),
               Status::Ok)) {
        auto const want = (basic.rights & Rights::Destroy) == Rights::Destroy
                              ? Status::NotSupported
                              : Status::AccessDenied;
        ok &= expect("shell: task_kill(the job) = ", ours::task_kill(job), want);
    } else {
        ok = false;
    }

    auto process = ours::Uandle::Invalid;
    auto region = ours::Uandle::Invalid;
    if (!expect("shell: process_create = ",
                ours::process_create(job, "killed", sizeof("killed") - 1,
                                     ours::ProcessCreateOptions::None, &process, &region),
                Status::Ok)) {
        auto _ = ort::debug_line("shell: task_kill checks FAILED");
        return;
    }
    auto weak = ours::Uandle::Invalid;
    if (expect("shell: handle_duplicate(a process, without Destroy) = ",
               ours::handle_duplicate(process, Rights::Basic, &weak), Status::Ok)) {
        ok &= expect("shell: task_kill(without Destroy) = ", ours::task_kill(weak),
                     Status::AccessDenied);
        ok &= expect("shell: close(that handle) = ", ours::handle_close(weak), Status::Ok);
    } else {
        ok = false;
    }
    ok &=
        expect("shell: task_kill(a process not started) = ", ours::task_kill(process), Status::Ok);
    ok &= expect("shell: wait_one(a process killed before it started, Terminated, poll) = ",
                 ours::object_wait_one(process, Signals::Terminated, 0, nullptr), Status::Ok);
    ok &= killed_by_task_kill(process);
    ok &= expect("shell: task_kill(a dead process) = ", ours::task_kill(process), Status::Ok);
    ok &= expect("shell: close(region) = ", ours::handle_close(region), Status::Ok);
    ok &= expect("shell: close(process) = ", ours::handle_close(process), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: task_kill checks passed");
    } else {
        auto _ = ort::debug_line("shell: task_kill checks FAILED");
    }
}

/// The second thread of check_pager(): reads the first word of the pager VMO
/// mapped at |at| -- a fault that waits until the pager supplies the page --
/// and writes what it read to the channel end |channel|. Nothing but system
/// calls, as second_thread().
[[noreturn]]
auto pager_reader(usize const channel, usize const at) -> void {
    usize const word = *reinterpret_cast<usize const volatile *>(at);
    auto _ = ours::channel_write(static_cast<ours::Uandle>(static_cast<u32>(channel)),
                                 ours::ChannelWriteOptions::None, &word, sizeof(word), nullptr, 0);
    ours::thread_exit();
}

/// The pager calls (step 10 of .agents/tasks/user-pager/), on both images: a
/// second thread faults on a mapped pager VMO, and this one serves the fault
/// as a pager would.
///
///   - making: pager_create with an option is InvArgs; pager_create_vmo
///     resolves both handles before its arguments -- none BadHandle, the
///     wrong object BadType, a port without Write AccessDenied -- then
///     refuses Resizable (NotSupported), an unknown option (InvArgs) and a
///     size past the largest (OutOfRange);
///   - the fault: the port gets one Read packet, with the key, for the page
///     the thread touched; pager_supply_pages moves a page written into an
///     aux VMO there, the thread wakes and reads it, and the aux VMO reads
///     zero after;
///   - supplying: a VMO not the pager's is InvArgs, an unaligned offset
///     InvArgs, a pager or aux VMO handle without Write AccessDenied, a
///     range past the end OutOfRange, a pager VMO as the aux NotSupported,
///     zero length Ok; a page supplied twice keeps the first;
///   - detaching: a VMO not the pager's is InvArgs; the port gets Complete,
///     and nothing after it; a second detach is Ok; a supply after it is
///     BadState, a read of a page never supplied BadState, and the page
///     supplied before still reads.
///
/// The waits are infinite, as check_thread_start()'s: a fault the pager
/// never hears of hangs the shell here rather than being reported.
auto check_pager(ours::Uandle const vma) -> void {
    using ours::Rights;
    using ours::Signals;
    using ours::Status;

    constexpr u64 PAGE = 4096;
    constexpr u64 VMO_SIZE = 2 * PAGE;
    constexpr u64 KEY = 0x9a6e'0000'0000'0010;
    constexpr usize VALUE = 0x9a6e'7da7'a000'0010;
    constexpr usize OTHER = 0x9a6e'7da7'a000'0011;

    if (vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: pager checks skipped (no VmaRoot)");
        return;
    }

    auto ok = true;
    auto pager = ours::Uandle::Invalid;
    ok &=
        expect("shell: pager_create(an option) = ", ours::pager_create(1, &pager), Status::InvArgs);

    auto port = ours::Uandle::Invalid;
    auto vmo = ours::Uandle::Invalid;
    auto aux = ours::Uandle::Invalid;
    auto here = ours::Uandle::Invalid;
    auto there = ours::Uandle::Invalid;
    auto const close_all = [&] {
        for (auto const handle: {pager, port, vmo, aux, here, there}) {
            if (handle != ours::Uandle::Invalid) {
                auto _ = ours::handle_close(handle);
            }
        }
    };
    if (!expect("shell: pager_create = ", ours::pager_create(0, &pager), Status::Ok) ||
        !expect("shell: port_create = ", ours::port_create(ours::PortCreateOptions::None, &port),
                Status::Ok) ||
        !expect("shell: vmo_create(an aux VMO) = ",
                ours::vmo_create(PAGE, ours::VmoOptions::None, &aux), Status::Ok) ||
        !expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, &here, &there),
                Status::Ok)) {
        close_all();
        auto _ = ort::debug_line("shell: pager checks FAILED");
        return;
    }

    // Making a pager VMO: the handles, then the arguments.
    ok &= expect("shell: pager_create_vmo(Uandle::Invalid) = ",
                 ours::pager_create_vmo(ours::Uandle::Invalid, ours::VmoOptions::None, port, KEY,
                                        VMO_SIZE, &vmo),
                 Status::BadHandle);
    ok &= expect("shell: pager_create_vmo(a port as the pager) = ",
                 ours::pager_create_vmo(port, ours::VmoOptions::None, port, KEY, VMO_SIZE, &vmo),
                 Status::BadType);
    ok &= expect("shell: pager_create_vmo(a pager as the port) = ",
                 ours::pager_create_vmo(pager, ours::VmoOptions::None, pager, KEY, VMO_SIZE, &vmo),
                 Status::BadType);
    auto weak = ours::Uandle::Invalid;
    if (expect("shell: handle_duplicate(the port, without Write) = ",
               ours::handle_duplicate(port, Rights::Read, &weak), Status::Ok)) {
        ok &=
            expect("shell: pager_create_vmo(a port without Write) = ",
                   ours::pager_create_vmo(pager, ours::VmoOptions::None, weak, KEY, VMO_SIZE, &vmo),
                   Status::AccessDenied);
        ok &= expect("shell: close(that handle) = ", ours::handle_close(weak), Status::Ok);
    } else {
        ok = false;
    }
    ok &= expect(
        "shell: pager_create_vmo(Resizable) = ",
        ours::pager_create_vmo(pager, ours::VmoOptions::Resizable, port, KEY, VMO_SIZE, &vmo),
        Status::NotSupported);
    ok &= expect(
        "shell: pager_create_vmo(an unknown option) = ",
        ours::pager_create_vmo(pager, static_cast<ours::VmoOptions>(1), port, KEY, VMO_SIZE, &vmo),
        Status::InvArgs);
    ok &= expect("shell: pager_create_vmo(the largest size and more) = ",
                 ours::pager_create_vmo(pager, ours::VmoOptions::None, port, KEY, ~u64{0}, &vmo),
                 Status::OutOfRange);

    ours::VirtAddr at{};
    SecondThread second;
    if (!expect("shell: pager_create_vmo = ",
                ours::pager_create_vmo(pager, ours::VmoOptions::None, port, KEY, VMO_SIZE, &vmo),
                Status::Ok) ||
        !expect("shell: vma_map(the pager VMO) = ",
                ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite, 0, vmo,
                              0, VMO_SIZE, &at),
                Status::Ok) ||
        !make_second_thread(vma, &second)) {
        if (at != ours::VirtAddr{0}) {
            auto _ = ours::vma_unmap(vma, at, VMO_SIZE);
        }
        close_all();
        auto _ = ort::debug_line("shell: pager checks FAILED");
        return;
    }

    // The fault, and the pager's answer to it.
    auto const channel = static_cast<usize>(static_cast<u32>(there));
    ok &= expect("shell: thread_start(a reader of the pager VMO) = ",
                 start_second_thread(second, &pager_reader, channel, std::to_underlying(at)),
                 Status::Ok);
    ours::PortPacket packet{};
    ok &= expect("shell: port_wait(the page request) = ",
                 ours::port_wait(port, ours::INSTANT_INFINITE, &packet), Status::Ok);
    if (packet.key != KEY || packet.type != ours::PacketType::PageRequest ||
        packet.page_request.command != ours::PageRequestCommand::Read ||
        packet.page_request.offset != 0 || packet.page_request.length != PAGE) {
        auto _ = ort::debug_line("shell: the page request is not a Read of the first page");
        ok = false;
    }

    ok &= expect("shell: vmo_write(the aux VMO) = ", ours::vmo_write(aux, &VALUE, 0, sizeof(VALUE)),
                 Status::Ok);
    // Refused before anything moves: the handles, then the pager's own checks.
    ok &= expect("shell: pager_supply_pages(Uandle::Invalid) = ",
                 ours::pager_supply_pages(ours::Uandle::Invalid, vmo, 0, PAGE, aux, 0),
                 Status::BadHandle);
    ok &= expect("shell: pager_supply_pages(into a VMO not the pager's) = ",
                 ours::pager_supply_pages(pager, aux, 0, PAGE, aux, 0), Status::InvArgs);
    ok &= expect("shell: pager_supply_pages(an unaligned offset) = ",
                 ours::pager_supply_pages(pager, vmo, 1, PAGE, aux, 0), Status::InvArgs);
    for (auto const which: {&vmo, &aux}) {
        if (expect("shell: handle_duplicate(a VMO, without Write) = ",
                   ours::handle_duplicate(*which, Rights::Read, &weak), Status::Ok)) {
            auto target = vmo;
            auto source = aux;
            if (which == &vmo) {
                target = weak;
            } else {
                source = weak;
            }
            ok &= expect("shell: pager_supply_pages(a VMO without Write) = ",
                         ours::pager_supply_pages(pager, target, 0, PAGE, source, 0),
                         Status::AccessDenied);
            ok &= expect("shell: close(that handle) = ", ours::handle_close(weak), Status::Ok);
        } else {
            ok = false;
        }
    }
    ok &= expect("shell: pager_supply_pages = ",
                 ours::pager_supply_pages(pager, vmo, 0, PAGE, aux, 0), Status::Ok);

    // What the thread read.
    auto seen = Signals::None;
    ok &= expect("shell: wait_one(the reader's channel, Readable) = ",
                 ours::object_wait_one(here, Signals::Readable, ours::INSTANT_INFINITE, &seen),
                 Status::Ok);
    usize word = 0;
    u32 bytes = 0;
    ok &= expect("shell: channel_read(what the reader read) = ",
                 ours::channel_read(here, ours::ChannelReadOptions::None, &word, nullptr,
                                    sizeof(word), 0, &bytes, nullptr),
                 Status::Ok);
    if (bytes != sizeof(word) || word != VALUE) {
        auto _ = ort::debug_line("shell: the reader did not read the supplied page");
        ok = false;
    }
    ok &= expect(
        "shell: wait_one(the reader, Terminated) = ",
        ours::object_wait_one(second.thread, Signals::Terminated, ours::INSTANT_INFINITE, &seen),
        Status::Ok);
    ok &= drop_second_thread(vma, second);

    word = 1;
    ok &= expect("shell: vmo_read(the aux VMO, after) = ",
                 ours::vmo_read(aux, &word, 0, sizeof(word)), Status::Ok);
    if (word != 0) {
        auto _ = ort::debug_line("shell: the aux VMO still has the page it supplied");
        ok = false;
    }

    // A second supply of the same page keeps the first.
    ok &= expect("shell: vmo_write(the aux VMO, again) = ",
                 ours::vmo_write(aux, &OTHER, 0, sizeof(OTHER)), Status::Ok);
    ok &= expect("shell: pager_supply_pages(a page supplied already) = ",
                 ours::pager_supply_pages(pager, vmo, 0, PAGE, aux, 0), Status::Ok);
    if (*reinterpret_cast<usize const volatile *>(at) != VALUE) {
        auto _ = ort::debug_line("shell: a second supply replaced the page");
        ok = false;
    }
    ok &= expect("shell: pager_supply_pages(zero length) = ",
                 ours::pager_supply_pages(pager, vmo, 0, 0, aux, 0), Status::Ok);
    ok &= expect("shell: pager_supply_pages(past the end) = ",
                 ours::pager_supply_pages(pager, vmo, VMO_SIZE, PAGE, aux, 0), Status::OutOfRange);
    ok &= expect("shell: pager_supply_pages(from a pager VMO) = ",
                 ours::pager_supply_pages(pager, vmo, PAGE, PAGE, vmo, 0), Status::NotSupported);

    // The detach.
    ok &= expect("shell: pager_detach_vmo(a VMO not the pager's) = ",
                 ours::pager_detach_vmo(pager, aux), Status::InvArgs);
    ok &= expect("shell: pager_detach_vmo = ", ours::pager_detach_vmo(pager, vmo), Status::Ok);
    packet = {};
    ok &= expect("shell: port_wait(the Complete) = ",
                 ours::port_wait(port, ours::INSTANT_INFINITE, &packet), Status::Ok);
    if (packet.key != KEY || packet.type != ours::PacketType::PageRequest ||
        packet.page_request.command != ours::PageRequestCommand::Complete) {
        auto _ = ort::debug_line("shell: the packet after the detach is not Complete");
        ok = false;
    }
    ok &= expect("shell: port_wait(after the Complete, poll) = ", ours::port_wait(port, 0, &packet),
                 Status::TimeOut);
    ok &=
        expect("shell: pager_detach_vmo(again) = ", ours::pager_detach_vmo(pager, vmo), Status::Ok);
    ok &= expect("shell: pager_supply_pages(after the detach) = ",
                 ours::pager_supply_pages(pager, vmo, PAGE, PAGE, aux, 0), Status::BadState);
    ok &= expect("shell: vmo_read(a page never supplied, after the detach) = ",
                 ours::vmo_read(vmo, &word, PAGE, sizeof(word)), Status::BadState);
    word = 0;
    ok &= expect("shell: vmo_read(the page supplied, after the detach) = ",
                 ours::vmo_read(vmo, &word, 0, sizeof(word)), Status::Ok);
    if (word != VALUE) {
        auto _ = ort::debug_line("shell: the supplied page went with the detach");
        ok = false;
    }

    ok &= expect("shell: vma_unmap(the pager VMO) = ", ours::vma_unmap(vma, at, VMO_SIZE),
                 Status::Ok);
    ok &= expect("shell: close(the pager VMO) = ", ours::handle_close(vmo), Status::Ok);
    ok &= expect("shell: close(the aux VMO) = ", ours::handle_close(aux), Status::Ok);
    ok &= expect("shell: close(the pager) = ", ours::handle_close(pager), Status::Ok);
    ok &= expect("shell: close(the port) = ", ours::handle_close(port), Status::Ok);
    ok &= expect("shell: close(here) = ", ours::handle_close(here), Status::Ok);
    ok &= expect("shell: close(there) = ", ours::handle_close(there), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: pager checks passed");
    } else {
        auto _ = ort::debug_line("shell: pager checks FAILED");
    }
}

/// Starts fault_probe running |probe|, as run_probe() does -- its
/// processargs written on the bootstrap channel, an answer channel as User0 --
/// but binds nothing to it and waits for nothing: the process goes to
/// |process| and this end of the answer channel to |answer|, both the
/// caller's to close. False, reported, when it did not start; nothing is left
/// behind then.
auto start_probe(ours::Uandle const job, std::string_view const probe,
                 shell::LaunchProgram const launcher, void *const launcher_context,
                 ours::Uandle *const process, ours::Uandle *const answer) -> bool {
    using ours::Status;

    u8 message[128];
    u32 length = 0;
    u32 const infos[] = {ours::handle_info(ours::HandleType::User0, 0),
                         ours::handle_info(ours::HandleType::VmaRoot, 0),
                         ours::handle_info(ours::HandleType::ProcSelf, 0)};
    std::string_view const args[] = {user_programs::FAULT_PROBE, {probe.data(), probe.size()}};
    if (auto const status = ours::encode_process_args(message, infos, args, {}, {}, &length);
        status != Status::Ok) {
        report("shell: encoding the probe's processargs = ", status);
        return false;
    }

    auto answer_there = ours::Uandle::Invalid;
    auto bootstrap = ours::Uandle::Invalid;
    auto bootstrap_there = ours::Uandle::Invalid;
    if (!expect("shell: probe channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, answer, &answer_there),
                Status::Ok)) {
        return false;
    }
    if (!expect(
            "shell: probe channel_create = ",
            ours::channel_create(ours::ChannelCreateOptions::None, &bootstrap, &bootstrap_there),
            Status::Ok)) {
        auto _ = ours::handle_close(*answer);
        auto _ = ours::handle_close(answer_there);
        return false;
    }

    auto const started =
        launcher(launcher_context, job, user_programs::FAULT_PROBE, bootstrap_there);
    if (started.is_err()) {
        report("shell: starting fault_probe = ", started.unwrap_err());
        auto _ = ours::handle_close(bootstrap);
        auto _ = ours::handle_close(*answer);
        auto _ = ours::handle_close(answer_there);
        return false;
    }
    *process = started.unwrap().process;

    // Its handle to itself, with every right, as run_probe() hands it.
    auto self = ours::Uandle::Invalid;
    if (auto const duplicated = ours::handle_duplicate(*process, ours::Rights::SameRights, &self);
        duplicated != Status::Ok) {
        report("shell: duplicating the probe's process handle = ", duplicated);
    }
    ours::Uandle handed[] = {answer_there, started.unwrap().root_vma, self};
    auto const written = ours::channel_write(bootstrap, ours::ChannelWriteOptions::None, message,
                                             length, handed, static_cast<u32>(std::size(handed)));
    auto _ = ours::handle_close(bootstrap);
    if (written != Status::Ok) {
        report("shell: writing the probe's processargs = ", written);
    }
    return true;
}

/// Starts fault_probe with a bootstrap channel on which no processargs ever
/// come: its runtime waits for them, in a system call, until that channel
/// closes. The process goes to |process| and this end of the channel to
/// |bootstrap|, both the caller's to close; closing the channel is what ends
/// a probe nothing killed. False, reported, when it did not start; nothing is
/// left behind then.
auto start_waiting_probe(ours::Uandle const job, shell::LaunchProgram const launcher,
                         void *const launcher_context, ours::Uandle *const process,
                         ours::Uandle *const bootstrap) -> bool {
    using ours::Status;

    auto bootstrap_there = ours::Uandle::Invalid;
    if (!expect("shell: channel_create = ",
                ours::channel_create(ours::ChannelCreateOptions::None, bootstrap, &bootstrap_there),
                Status::Ok)) {
        return false;
    }
    auto const started =
        launcher(launcher_context, job, user_programs::FAULT_PROBE, bootstrap_there);
    if (started.is_err()) {
        report("shell: starting fault_probe = ", started.unwrap_err());
        auto _ = ours::handle_close(*bootstrap);
        return false;
    }
    *process = started.unwrap().process;
    auto _ = ours::handle_close(started.unwrap().root_vma);
    return true;
}

/// Kills |process| with task_kill, the call reported as |what|, and waits for
/// it to end. The wait is infinite: a kill that does not land hangs the shell
/// here. True when the call answered Ok and the process ended with
/// TASK_RETCODE_SYSCALL_KILL.
auto kill_and_wait(ours::Uandle const process, std::string_view const what) -> bool {
    using ours::Status;

    auto ok = expect(what, ours::task_kill(process), Status::Ok);
    ok &= expect(
        "shell: wait_one(the killed process, Terminated) = ",
        ours::object_wait_one(process, ours::Signals::Terminated, ours::INSTANT_INFINITE, nullptr),
        Status::Ok);
    return ok && killed_by_task_kill(process);
}

/// A probe blocked in a system call, its runtime's wait for processargs. It
/// is given a while to get there, so the kill most likely ends that wait
/// rather than landing before it.
auto kill_waiting_probe(ours::Uandle const job, shell::LaunchProgram const launcher,
                        void *const launcher_context) -> bool {
    using ours::Status;

    auto process = ours::Uandle::Invalid;
    auto bootstrap = ours::Uandle::Invalid;
    if (!start_waiting_probe(job, launcher, launcher_context, &process, &bootstrap)) {
        return false;
    }
    spin(SPIN_ROUNDS);
    auto ok = kill_and_wait(process, "shell: task_kill(a process waiting for its processargs) = ");
    ok &= expect("shell: close(that process) = ", ours::handle_close(process), Status::Ok);
    ok &= expect("shell: close(its bootstrap) = ", ours::handle_close(bootstrap), Status::Ok);
    return ok;
}

/// A probe whose only thread is suspended. A debugger channel on |job| hands
/// out the probe's first thread as it starts (ProcessStarting, then
/// exception_get_thread): the one way this shell has to a thread of a
/// process it launched. The start is let go, and the thread suspended --
/// most likely in its runtime's wait, given a while to get there. The kill
/// comes once the thread reads ThreadSuspended, so it lands on a thread that
/// has stopped, which leaves its stop only to die. Closing the token after
/// that resumes nothing.
auto kill_suspended_probe(ours::Uandle const job, shell::LaunchProgram const launcher,
                          void *const launcher_context) -> bool {
    using ours::Signals;
    using ours::Status;

    auto debugger = ours::Uandle::Invalid;
    if (!expect("shell: task_create_exception_channel(job, Debugger) = ",
                ours::task_create_exception_channel(job, ours::ExceptionChannelOptions::Debugger,
                                                    &debugger),
                Status::Ok)) {
        return false;
    }
    auto process = ours::Uandle::Invalid;
    auto bootstrap = ours::Uandle::Invalid;
    if (!start_waiting_probe(job, launcher, launcher_context, &process, &bootstrap)) {
        auto _ = ours::handle_close(debugger);
        return false;
    }

    // The probe's start, which is the one start on dbg_1 while this runs.
    // Closing the exception lets it go on; closing the channel keeps it from
    // holding up any start after this one.
    auto ok = true;
    auto thread = ours::Uandle::Invalid;
    ours::ExceptionInfo info{};
    auto exception = ours::Uandle::Invalid;
    if (read_exception(debugger, &info, &exception)) {
        if (info.type == ours::ExceptionType::ProcessStarting && info.pid == koid_of(process)) {
            ok &= expect("shell: exception_get_thread(the probe's start) = ",
                         ours::exception_get_thread(exception, &thread), Status::Ok);
        } else {
            auto _ = ort::debug_line("shell: the debugger heard something other than the "
                                     "probe's start");
            ok = false;
        }
        auto _ = ours::handle_close(exception);
    } else {
        ok = false;
    }
    auto _ = ours::handle_close(debugger);

    auto token = ours::Uandle::Invalid;
    if (thread != ours::Uandle::Invalid) {
        spin(SPIN_ROUNDS);
        ok &=
            expect("shell: task_suspend(the probe's thread) = ", ours::task_suspend(thread, &token),
                   Status::Ok);
    }
    if (token != ours::Uandle::Invalid) {
        ok &= expect("shell: wait_one(the probe's thread, ThreadSuspended) = ",
                     ours::object_wait_one(thread, Signals::ThreadSuspended, ours::INSTANT_INFINITE,
                                           nullptr),
                     Status::Ok);
        ok &= kill_and_wait(process, "shell: task_kill(a process whose thread is suspended) = ");
        ok &= expect("shell: wait_one(its thread, Terminated, poll) = ",
                     ours::object_wait_one(thread, Signals::Terminated, 0, nullptr), Status::Ok);
        ok &= expect("shell: close(the dead thread's token) = ", ours::handle_close(token),
                     Status::Ok);
    }
    // A probe not killed because something above failed ends itself once its
    // bootstrap closes.
    auto _ = ours::handle_close(thread);
    ok &= expect("shell: close(that process) = ", ours::handle_close(process), Status::Ok);
    ok &= expect("shell: close(its bootstrap) = ", ours::handle_close(bootstrap), Status::Ok);
    return ok;
}

/// A probe that handles its own exceptions and has faulted
/// (own_exception_channel), so that it waits for itself for good: the answer
/// it waits for could only come from the thread that is waiting, and only a
/// kill from outside ends it. It says it is bound just before it faults, and
/// is given a while to fault, so the kill most likely ends that wait.
auto kill_probe_waiting_for_itself(ours::Uandle const job, shell::LaunchProgram const launcher,
                                   void *const launcher_context) -> bool {
    using ours::Signals;
    using ours::Status;

    auto process = ours::Uandle::Invalid;
    auto answer = ours::Uandle::Invalid;
    if (!start_probe(job, "own_exception_channel", launcher, launcher_context, &process, &answer)) {
        return false;
    }
    char said[16] = {};
    u32 bytes = 0;
    auto ok =
        expect("shell: wait_one(the probe's answer, Readable) = ",
               ours::object_wait_one(answer, Signals::Readable, ours::INSTANT_INFINITE, nullptr),
               Status::Ok);
    ok &= expect("shell: channel_read(the probe's answer) = ",
                 ours::channel_read(answer, ours::ChannelReadOptions::None, said, nullptr,
                                    sizeof(said), 0, &bytes, nullptr),
                 Status::Ok);
    if (std::string_view{said, bytes} != "bound") {
        auto _ = ort::debug_line({"shell: own_exception_channel answered \"",
                                  std::string_view{said, bytes}, "\", not \"bound\""});
        ok = false;
    }
    spin(SPIN_ROUNDS);
    ok &= kill_and_wait(process, "shell: task_kill(a process waiting for itself) = ");
    ok &= expect("shell: close(that process) = ", ours::handle_close(process), Status::Ok);
    ok &= expect("shell: close(its answer channel) = ", ours::handle_close(answer), Status::Ok);
    return ok;
}

/// A probe that kills itself (kill_self). The call does not return, so the
/// probe never answers; it ends with TASK_RETCODE_SYSCALL_KILL, its thread
/// dying on its way out of the call.
auto probe_killing_itself(ours::Uandle const job, shell::LaunchProgram const launcher,
                          void *const launcher_context) -> bool {
    using ours::Signals;
    using ours::Status;

    auto process = ours::Uandle::Invalid;
    auto answer = ours::Uandle::Invalid;
    if (!start_probe(job, "kill_self", launcher, launcher_context, &process, &answer)) {
        return false;
    }
    auto ok =
        expect("shell: wait_one(a process killing itself, Terminated) = ",
               ours::object_wait_one(process, Signals::Terminated, ours::INSTANT_INFINITE, nullptr),
               Status::Ok);
    ok &= killed_by_task_kill(process);
    // Its end of the channel went with it, nothing written.
    ok &= expect("shell: wait_one(its answer, Readable, poll) = ",
                 ours::object_wait_one(answer, Signals::Readable, 0, nullptr), Status::TimeOut);
    ok &= expect("shell: close(that process) = ", ours::handle_close(process), Status::Ok);
    ok &= expect("shell: close(its answer channel) = ", ours::handle_close(answer), Status::Ok);
    return ok;
}

/// task_kill on processes that run, where this shell can start fault_probe
/// (dbg_1): each ends with TASK_RETCODE_SYSCALL_KILL, wherever its thread
/// was when the kill came -- blocked in a system call
/// (kill_waiting_probe()), suspended (kill_suspended_probe()), waiting for
/// its own exception to be answered (kill_probe_waiting_for_itself()), or
/// in the kill itself (probe_killing_itself()).
auto check_task_kill_probes(ours::Uandle const job, shell::LaunchProgram const launcher,
                            void *const launcher_context) -> void {
    auto ok = kill_waiting_probe(job, launcher, launcher_context);
    ok &= kill_suspended_probe(job, launcher, launcher_context);
    ok &= kill_probe_waiting_for_itself(job, launcher, launcher_context);
    ok &= probe_killing_itself(job, launcher, launcher_context);

    if (ok) {
        auto _ = ort::debug_line("shell: task_kill probe checks passed");
    } else {
        auto _ = ort::debug_line("shell: task_kill probe checks FAILED");
    }
}

/// Where the vDSO's code lies in its image. Offsets and sizes are in bytes,
/// each a whole number of pages.
struct VdsoLayout {
    /// The code segment's offset in the image, which is its offset in the VMO
    /// too, and its size.
    u64 code;
    u64 code_size;
    /// The image, from its first byte to the end of its last segment.
    u64 whole;
};

/// The layout of the vDSO |vdso| names, read from its ELF headers; nothing
/// when they cannot be read or do not show exactly one executable segment.
///
/// The vDSO's VMO is its file, so the whole of it is read and parsed. Not only
/// its first page, where the headers are: the parser checks each segment's
/// file range against the bytes it is given, and the code segment lies past
/// the first page once the vDSO has grown past one (as it did with
/// object_get_child).
auto read_vdso_layout(ours::Uandle const vdso) -> ustl::Option<VdsoLayout> {
    constexpr auto PAGE = u64{4096};

    static u8 IMAGE[64 * 1024];
    u64 size = 0;
    if (!expect("shell: vmo_get_size(vdso) = ", ours::vmo_get_size(vdso, &size),
                ours::Status::Ok) ||
        size > sizeof(IMAGE)) {
        return ustl::NONE;
    }
    if (!expect("shell: vmo_read(vdso, its file) = ", ours::vmo_read(vdso, IMAGE, 0, size),
                ours::Status::Ok)) {
        return ustl::NONE;
    }
    auto const program = launch::Program::read({IMAGE, size});
    if (!program.is_ok()) {
        return ustl::NONE;
    }
    auto layout = VdsoLayout{.code = 0, .code_size = 0, .whole = 0};
    usize executable = 0;
    for (auto const &segment: program.unwrap().segments()) {
        auto const first = segment.vaddr & ~(PAGE - 1);
        auto const end = (segment.vaddr + segment.mem_size + PAGE - 1) & ~(PAGE - 1);
        layout.whole = std::max(layout.whole, end);
        if ((segment.flags & launch::PF_X) != 0) {
            layout.code = first;
            layout.code_size = end - first;
            executable += 1;
        }
    }
    if (executable != 1 || layout.code == 0) {
        return ustl::NONE;
    }
    return ustl::some(layout);
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
///   - an address the kernel cannot write back: InvArgs, and the mapping
///     made is taken down, sealed as it is, with the claim it took and the
///     range it registered. The same call with an address the kernel can
///     write is then Ok, the first registration and not a second; and a
///     process whose only executable mapping of the code failed that way has
///     no vDSO to start with. An ordinary sealed mapping is taken down the
///     same way, and its place is free for the next call;
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

    auto const layout = read_vdso_layout(vdso);
    if (layout.is_none()) {
        auto _ = ort::debug_line("shell: vdso map checks FAILED (its code not found)");
        return;
    }
    auto const code = layout->code;
    auto const code_size = layout->code_size;
    auto const whole = layout->whole;
    char code_text[10];
    char pages_text[10];
    auto _ = ort::debug_line(
        {"shell: the vDSO is ", ort::decimal(static_cast<u32>(whole / PAGE), pages_text),
         " page(s), its code at page ", ort::decimal(static_cast<u32>(code / PAGE), code_text)});

    ours::VirtAddr at{};
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
    // An address the kernel cannot write back: the call fails, and takes the
    // sealed mapping it made down again, with the claim. Had the claim stayed,
    // the call after it would be a second registration, AccessDenied.
    ok &= expect("shell: vma_map(vdso code, RX, sealed, mapped_addr = kernel half) = ",
                 ours::vma_map(p_vma, SEALED_RX, 0, vdso, code, code_size,
                               reinterpret_cast<ours::VirtAddr *>(KERNEL)),
                 Status::InvArgs);
    ok &= expect("shell: vma_map(vdso code, RX, sealed) = ",
                 ours::vma_map(p_vma, SEALED_RX, 0, vdso, code, code_size, &at), Status::Ok);

    ours::VirtAddr again{};
    ok &= expect("shell: vma_map(vdso code) a second time = ",
                 ours::vma_map(p_vma, SEALED_RX, 0, vdso, code, code_size, &again),
                 Status::AccessDenied);
    ok &= expect("shell: vma_map(vdso code) into another process = ",
                 ours::vma_map(q_vma, SEALED_RX, 0, vdso, code, code_size, &again), Status::Ok);

    // The range goes with the claim: s, whose one executable mapping of the
    // code fails that way, has no vDSO when process_start asks below. Had the
    // range stayed registered, s would start.
    ok &= expect("shell: vma_map(vdso code into s, RX, sealed, mapped_addr = kernel half) = ",
                 ours::vma_map(s_vma, SEALED_RX, 0, vdso, code, code_size,
                               reinterpret_cast<ours::VirtAddr *>(KERNEL)),
                 Status::InvArgs);

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
    // checked by check_vma_unmap() for an unmap, and only in the kernel
    // (vmaseal.test) for a change of protection, since no program can
    // protect yet.
    ours::Uandle plain = ours::Uandle::Invalid;
    ok &= expect("shell: vmo_create = ", ours::vmo_create(PAGE, ours::VmoOptions::None, &plain),
                 Status::Ok);
    ok &= expect(
        "shell: vma_map(an ordinary VMO, sealed) = ",
        ours::vma_map(q_vma, VmOptions::PermRead | VmOptions::Sealed, 0, plain, 0, PAGE, &again),
        Status::Ok);
    // Nor is the undo the vDSO's alone: an ordinary sealed mapping whose
    // address cannot be written back is taken down too, so the place it took
    // is free for the next call. Had it stayed, that call would find the place
    // taken, and no unmap could ever free it.
    constexpr auto SEALED_SPECIFIC_R =
        VmOptions::PermRead | VmOptions::Sealed | VmOptions::Specific;
    constexpr auto PLACE = usize{1} << 30;
    ok &= expect("shell: vma_map(an ordinary VMO, sealed, Specific, mapped_addr = kernel half) = ",
                 ours::vma_map(q_vma, SEALED_SPECIFIC_R, PLACE, plain, 0, PAGE,
                               reinterpret_cast<ours::VirtAddr *>(KERNEL)),
                 Status::InvArgs);
    ok &=
        expect("shell: vma_map(an ordinary VMO, sealed, Specific, the same place) = ",
               ours::vma_map(q_vma, SEALED_SPECIFIC_R, PLACE, plain, 0, PAGE, &again), Status::Ok);
    ok &= expect("shell: close(the VMO) = ", ours::handle_close(plain), Status::Ok);

    ours::Uandle t = ours::Uandle::Invalid;
    ok &= expect(
        "shell: thread_create = ",
        ours::thread_create(s, "first", sizeof("first") - 1, ours::ThreadCreateOptions::None, &t),
        Status::Ok);
    ok &= expect("shell: process_start(the vDSO's code undone, a slice mapped, no vDSO) = ",
                 ours::process_start(s, t, ours::VirtAddr{0x400000}, ours::VirtAddr{0x500000},
                                     ours::Uandle::Invalid, 0),
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

/// The sealed half of check_vma_unmap(), on a process made here and never
/// started, since a sealed mapping could never be taken out of this program
/// again. Everything is placed at a chosen offset of that process's root
/// region, where nothing else is, so that what neighbours what is known.
/// Answers whether every check passed.
auto check_unmap_sealed(ours::Uandle const job, ours::Uandle const vdso) -> bool {
    using ours::Status;
    using ours::VmOptions;

    constexpr auto PAGE = u64{4096};
    constexpr auto IMAGE = usize{1} << 30;
    constexpr auto ROW = IMAGE + 64 * PAGE;
    constexpr auto HOLDER = IMAGE + 128 * PAGE;
    constexpr auto SPECIFIC_R = VmOptions::PermRead | VmOptions::Specific;
    constexpr auto SEALED_R = SPECIFIC_R | VmOptions::Sealed;
    constexpr auto SEALED_RX = SEALED_R | VmOptions::PermExecute;
    constexpr auto SUB_REGION =
        VmOptions::Specific | VmOptions::CanMapRead | VmOptions::CanMapSpecific;

    auto const layout = read_vdso_layout(vdso);
    if (layout.is_none()) {
        auto _ = ort::debug_line("shell: the vDSO's code was not found");
        return false;
    }
    auto const code = layout->code;
    auto const code_size = layout->code_size;
    auto const whole = layout->whole;

    ours::Uandle process = ours::Uandle::Invalid;
    ours::Uandle region = ours::Uandle::Invalid;
    ours::Uandle plain = ours::Uandle::Invalid;
    auto ok = expect("shell: process_create = ",
                     ours::process_create(job, "unmap", sizeof("unmap") - 1,
                                          ours::ProcessCreateOptions::None, &process, &region),
                     Status::Ok) &&
              expect("shell: vmo_create(3 pages) = ",
                     ours::vmo_create(3 * PAGE, ours::VmoOptions::None, &plain), Status::Ok);

    // The vDSO, laid out as a launcher lays it out: the image read-only and its
    // code at its offset, read and execute, and sealed, which the kernel
    // insists on.
    ours::VirtAddr at{};
    ours::VirtAddr code_at{};
    if (ok) {
        ok &= expect("shell: vma_map(the vDSO before its code, R) = ",
                     ours::vma_map(region, SPECIFIC_R, IMAGE, vdso, 0, code, &at), Status::Ok);
        ok &=
            expect("shell: vma_map(the vDSO's code, RX, sealed) = ",
                   ours::vma_map(region, SEALED_RX, IMAGE + code, vdso, code, code_size, &code_at),
                   Status::Ok);
        if (whole > code + code_size) {
            ok &= expect("shell: vma_map(the vDSO after its code, R) = ",
                         ours::vma_map(region, SPECIFIC_R, IMAGE + code + code_size, vdso,
                                       code + code_size, whole - code - code_size, &at),
                         Status::Ok);
        }
    }
    if (code_at != ours::VirtAddr{0}) {
        auto const image_at = code_at - code;
        ok &= expect("shell: vma_unmap(the vDSO's whole image) = ",
                     ours::vma_unmap(region, image_at, whole), Status::AccessDenied);
        ok &= expect("shell: vma_unmap(the vDSO's code) = ",
                     ours::vma_unmap(region, code_at, code_size), Status::AccessDenied);
        ok &= expect("shell: vma_unmap(the vDSO's code and the page after it) = ",
                     ours::vma_unmap(region, code_at, code_size + PAGE), Status::AccessDenied);
        // The refusal is the code's, not the image's: the rest is not sealed.
        ok &= expect("shell: vma_unmap(the vDSO before its code) = ",
                     ours::vma_unmap(region, image_at, code), Status::Ok);
        ok &= expect("shell: vma_map(R, Specific, over the vDSO's code) = ",
                     ours::vma_map(region, SPECIFIC_R, IMAGE + code, plain, 0, PAGE, &at),
                     Status::AlreadyExists);
    }

    // An ordinary sealed mapping of two pages, an unsealed page on each side.
    // Any range that touches it is refused whole, before anything is torn
    // down: the page below is still there after three refusals that covered it.
    ours::VirtAddr below{};
    ours::VirtAddr sealed{};
    ours::VirtAddr above{};
    if (ok) {
        ok &= expect("shell: vma_map(R, Specific, the page below) = ",
                     ours::vma_map(region, SPECIFIC_R, ROW, plain, 0, PAGE, &below), Status::Ok);
        ok &= expect("shell: vma_map(R, Specific, sealed, 2 pages) = ",
                     ours::vma_map(region, SEALED_R, ROW + PAGE, plain, 0, 2 * PAGE, &sealed),
                     Status::Ok);
        ok &= expect("shell: vma_map(R, Specific, the page above) = ",
                     ours::vma_map(region, SPECIFIC_R, ROW + 3 * PAGE, plain, 0, PAGE, &above),
                     Status::Ok);
    }
    if (below != ours::VirtAddr{0} && sealed != ours::VirtAddr{0} && above != ours::VirtAddr{0}) {
        ok &= expect("shell: vma_unmap(the sealed mapping's second page) = ",
                     ours::vma_unmap(region, sealed + PAGE, PAGE), Status::AccessDenied);
        ok &= expect("shell: vma_unmap(the page below and the sealed mapping's first) = ",
                     ours::vma_unmap(region, below, 2 * PAGE), Status::AccessDenied);
        ok &= expect("shell: vma_unmap(the sealed mapping's second page and the page above) = ",
                     ours::vma_unmap(region, sealed + PAGE, 2 * PAGE), Status::AccessDenied);
        ok &=
            expect("shell: vma_unmap(all four pages) = ", ours::vma_unmap(region, below, 4 * PAGE),
                   Status::AccessDenied);
        ok &= expect("shell: vma_map(R, Specific, over the page below, after the refusals) = ",
                     ours::vma_map(region, SPECIFIC_R, ROW, plain, 0, PAGE, &at),
                     Status::AlreadyExists);
        ok &= expect("shell: vma_unmap(the page below) = ", ours::vma_unmap(region, below, PAGE),
                     Status::Ok);
        ok &= expect("shell: vma_unmap(the page above) = ", ours::vma_unmap(region, above, PAGE),
                     Status::Ok);
    }

    // A sub-region with a sealed mapping in it: destroying the sub-region would
    // take the mapping with it, so that is refused as well, and the sub-region
    // goes on taking mappings.
    ours::Uandle holder = ours::Uandle::Invalid;
    ours::VirtAddr holder_at{};
    if (ok) {
        ok &= expect("shell: vma_allocate(Specific, 2 pages) = ",
                     ours::vma_allocate(region, SUB_REGION, HOLDER, 2 * PAGE, &holder, &holder_at),
                     Status::Ok);
        ok &= expect("shell: vma_map(R, sealed, into the sub-region) = ",
                     ours::vma_map(holder, SEALED_R, 0, plain, 0, PAGE, &at), Status::Ok);
        ok &= expect("shell: vma_unmap(the sub-region holding it, through its parent) = ",
                     ours::vma_unmap(region, holder_at, 2 * PAGE), Status::AccessDenied);
        ok &= expect("shell: vma_map(R, into the sub-region, after the refusal) = ",
                     ours::vma_map(holder, SPECIFIC_R, PAGE, plain, 0, PAGE, &at), Status::Ok);
    }

    // Closing the last handle to a process never started takes it, and its
    // address space, away (check_process_create()). Handles that were never
    // made answer BadHandle, which is no news by then.
    for (auto const handle: {holder, plain, region, process}) {
        if (handle != ours::Uandle::Invalid) {
            ok &= ours::handle_close(handle) == Status::Ok;
        }
    }
    return ok;
}

/// vma_unmap (component-framework step 9): what is unmapped goes and its range
/// comes back, and nothing sealed, nothing outside the region and nothing the
/// handle may not reach goes with it.
///
/// Runs in every image, on this program's own root region |vma|, in a stretch
/// of it no other check uses; and, where it is handed the root job and the
/// vDSO, on a process made here for what is sealed (check_unmap_sealed()).
/// What it pins down:
///   - a mapping unmapped whole leaves its range free: the same Specific
///     request lands there again, and a load through it reads the new VMO,
///     not the old one that the first load put in the TLB;
///   - a range with nothing in it is Ok;
///   - part of a mapping: its first and last pages go, the middle still
///     reads, and unmapping the rest frees the whole range;
///   - a sub-region: through its own handle, a range past its end, before its
///     start or running out of it is InvArgs, and its mapping can be unmapped;
///     through the root's, a range over half of it is InvArgs, one over all of
///     it is InvArgs from a handle without OpChildren and destroys it from one
///     with it, after which its handle answers BadState and its range can be
///     reserved again;
///   - the arguments: an unaligned address, a length of 0, one too large to
///     round up and the kernel half are InvArgs; a VMO's handle is BadType, a
///     value that names nothing BadHandle;
///   - sealed mappings: the vDSO's code, whole or with a neighbour, and part of
///     or a range straddling an ordinary sealed mapping, are AccessDenied with
///     nothing torn down; a sub-region holding one cannot be destroyed.
auto check_vma_unmap(ours::Uandle const vma, ours::Uandle const job, ours::Uandle const vdso)
    -> void {
    using ours::Status;
    using ours::VmoOptions;
    using ours::VmOptions;

    constexpr auto PAGE = u64{4096};
    // Past check_vma_allocate()'s stretch, which ends 113 pages above 1 GiB.
    constexpr auto SPOT = (usize{1} << 30) + 256 * PAGE;
    constexpr auto TRIMMED = SPOT + 2 * PAGE;
    constexpr auto REGION = SPOT + 8 * PAGE;
    constexpr auto SPECIFIC_R = VmOptions::PermRead | VmOptions::Specific;
    constexpr auto SUB_REGION =
        VmOptions::Specific | VmOptions::CanMapRead | VmOptions::CanMapSpecific;

    if (vma == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vma_unmap checks FAILED (no VmaRoot)");
        return;
    }

    // One VMO of a page and one of three, each page marked in its first byte.
    auto ok = true;
    ours::Uandle one = ours::Uandle::Invalid;
    ours::Uandle three = ours::Uandle::Invalid;
    u8 const marks[4] = {0xa1, 0xb0, 0xb1, 0xb2};
    ok &= expect("shell: vmo_create(1 page) = ", ours::vmo_create(PAGE, VmoOptions::None, &one),
                 Status::Ok);
    ok &= expect("shell: vmo_create(3 pages) = ",
                 ours::vmo_create(3 * PAGE, VmoOptions::None, &three), Status::Ok);
    ok &= ours::vmo_write(one, &marks[0], 0, 1) == Status::Ok;
    for (u64 page = 0; page < 3; ++page) {
        ok &= ours::vmo_write(three, &marks[1 + page], page * PAGE, 1) == Status::Ok;
    }
    auto const byte_at = [](ours::VirtAddr const addr) -> u8 {
        return *reinterpret_cast<u8 const volatile *>(addr);
    };

    // Whole: the first load puts the page in the TLB, and the load after the
    // range is mapped again must not find it there.
    ours::VirtAddr first{};
    ok &= expect("shell: vma_map(R, Specific) = ",
                 ours::vma_map(vma, SPECIFIC_R, SPOT, one, 0, PAGE, &first), Status::Ok);
    if (!ok || first == ours::VirtAddr{0}) {
        auto _ = ort::debug_line("shell: vma_unmap checks FAILED");
        return;
    }
    // Where the root region starts, to aim the requests below.
    auto const root_base = first - SPOT;
    ok &= byte_at(first) == marks[0];
    ok &= expect("shell: vma_unmap(the mapping) = ", ours::vma_unmap(vma, first, PAGE), Status::Ok);
    ok &=
        expect("shell: vma_unmap(the same range, now empty) = ", ours::vma_unmap(vma, first, PAGE),
               Status::Ok);
    ours::VirtAddr again{};
    ok &= expect("shell: vma_map(R, Specific, the same place) = ",
                 ours::vma_map(vma, SPECIFIC_R, SPOT, three, 0, PAGE, &again), Status::Ok);
    if (again != first) {
        auto _ = ort::debug_line("shell: the unmapped place could not be mapped again");
        ok = false;
    } else if (byte_at(again) != marks[1]) {
        auto _ = ort::debug_line("shell: a load through the new mapping read the old page");
        ok = false;
    }
    ok &= expect("shell: vma_unmap(it) = ", ours::vma_unmap(vma, first, PAGE), Status::Ok);

    // Part: the pages inside the range go and the rest stays. The mapping
    // keeps its range until all of it is gone, and then the whole is free.
    ours::VirtAddr pages{};
    ok &= expect("shell: vma_map(R, Specific, 3 pages) = ",
                 ours::vma_map(vma, SPECIFIC_R, TRIMMED, three, 0, 3 * PAGE, &pages), Status::Ok);
    if (pages == root_base + TRIMMED) {
        ok &= expect("shell: vma_unmap(its last page) = ",
                     ours::vma_unmap(vma, pages + 2 * PAGE, PAGE), Status::Ok);
        ok &= expect("shell: vma_unmap(its first page) = ", ours::vma_unmap(vma, pages, PAGE),
                     Status::Ok);
        if (byte_at(pages + PAGE) != marks[2]) {
            auto _ = ort::debug_line("shell: the middle page did not stay mapped");
            ok = false;
        }
        ok &= expect("shell: vma_unmap(all 3 pages) = ", ours::vma_unmap(vma, pages, 3 * PAGE),
                     Status::Ok);
        ok &=
            expect("shell: vma_map(R, Specific, the same 3 pages) = ",
                   ours::vma_map(vma, SPECIFIC_R, TRIMMED, three, 0, 3 * PAGE, &pages), Status::Ok);
        ok &=
            expect("shell: vma_unmap(them) = ", ours::vma_unmap(vma, pages, 3 * PAGE), Status::Ok);
    } else {
        ok = false;
    }

    // A sub-region: what its own handle reaches, and what the root's may do
    // to it.
    ours::Uandle region = ours::Uandle::Invalid;
    ours::VirtAddr region_at{};
    ok &= expect("shell: vma_allocate(Specific, 2 pages) = ",
                 ours::vma_allocate(vma, SUB_REGION, REGION, 2 * PAGE, &region, &region_at),
                 Status::Ok);
    if (region_at == root_base + REGION) {
        ours::VirtAddr inner{};
        ok &= expect("shell: vma_map(R, page 0 of the sub-region) = ",
                     ours::vma_map(region, SPECIFIC_R, 0, one, 0, PAGE, &inner), Status::Ok);
        ok &= expect("shell: vma_unmap(the sub-region, past its end) = ",
                     ours::vma_unmap(region, region_at + 2 * PAGE, PAGE), Status::InvArgs);
        ok &= expect("shell: vma_unmap(the sub-region, from before its start) = ",
                     ours::vma_unmap(region, region_at - PAGE, 2 * PAGE), Status::InvArgs);
        ok &= expect("shell: vma_unmap(the sub-region, running out of it) = ",
                     ours::vma_unmap(region, region_at + PAGE, 2 * PAGE), Status::InvArgs);
        ok &= expect("shell: vma_unmap(the sub-region's mapping, through its handle) = ",
                     ours::vma_unmap(region, inner, PAGE), Status::Ok);
        ok &= expect("shell: vma_map(R, page 0 of the sub-region, again) = ",
                     ours::vma_map(region, SPECIFIC_R, 0, one, 0, PAGE, &inner), Status::Ok);
        ok &= expect("shell: vma_unmap(half the sub-region, through the root) = ",
                     ours::vma_unmap(vma, region_at, PAGE), Status::InvArgs);

        // The root's handle has OpChildren; a copy without it reaches the
        // mappings beside a sub-region and not the sub-region.
        ours::Uandle no_children = ours::Uandle::Invalid;
        ok &= expect(
            "shell: handle_duplicate(vma, without OpChildren) = ",
            ours::handle_duplicate(vma, ours::Rights::Read | ours::Rights::Write, &no_children),
            Status::Ok);
        ok &= expect("shell: vma_unmap(the whole sub-region, without OpChildren) = ",
                     ours::vma_unmap(no_children, region_at, 2 * PAGE), Status::InvArgs);
        ok &=
            expect("shell: close(the copy without OpChildren) = ", ours::handle_close(no_children),
                   Status::Ok);
        ok &= expect("shell: vma_unmap(the whole sub-region) = ",
                     ours::vma_unmap(vma, region_at, 2 * PAGE), Status::Ok);

        ours::VirtAddr refused{};
        ok &=
            expect("shell: vma_map(into the destroyed sub-region) = ",
                   ours::vma_map(region, SPECIFIC_R, 0, one, 0, PAGE, &refused), Status::BadState);
        ok &= expect("shell: vma_unmap(in the destroyed sub-region) = ",
                     ours::vma_unmap(region, region_at, PAGE), Status::BadState);
        ours::Uandle reserved = ours::Uandle::Invalid;
        ours::VirtAddr reserved_at{};
        ok &= expect("shell: vma_allocate(Specific, where the sub-region was) = ",
                     ours::vma_allocate(vma, SUB_REGION, REGION, 2 * PAGE, &reserved, &reserved_at),
                     Status::Ok);
        ok &= expect("shell: vma_unmap(it) = ", ours::vma_unmap(vma, reserved_at, 2 * PAGE),
                     Status::Ok);
        ok &= expect("shell: close(it) = ", ours::handle_close(reserved), Status::Ok);
    } else {
        ok = false;
    }
    ok &=
        expect("shell: close(the destroyed sub-region) = ", ours::handle_close(region), Status::Ok);

    // The arguments, each aimed at the stretch above, which is empty by now.
    ok &= expect("shell: vma_unmap(unaligned address) = ",
                 ours::vma_unmap(vma, root_base + SPOT + 8, PAGE), Status::InvArgs);
    ok &= expect("shell: vma_unmap(length 0) = ", ours::vma_unmap(vma, root_base + SPOT, 0),
                 Status::InvArgs);
    ok &= expect("shell: vma_unmap(a length too large to round up) = ",
                 ours::vma_unmap(vma, root_base + SPOT, ~usize{0}), Status::InvArgs);
    ok &= expect("shell: vma_unmap(the kernel half) = ", ours::vma_unmap(vma, KERNEL, PAGE),
                 Status::InvArgs);
    ok &= expect("shell: vma_unmap(vma = a VMO) = ", ours::vma_unmap(one, root_base + SPOT, PAGE),
                 Status::BadType);
    ok &= expect("shell: vma_unmap(vma = nothing) = ",
                 ours::vma_unmap(ours::Uandle::Invalid, root_base + SPOT, PAGE), Status::BadHandle);

    if (job == ours::Uandle::Invalid || vdso == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: vma_unmap of sealed mappings skipped (no root job or "
                                 "vDSO)");
    } else {
        ok &= check_unmap_sealed(job, vdso);
    }

    ok &= expect("shell: close(the 1-page VMO) = ", ours::handle_close(one), Status::Ok);
    ok &= expect("shell: close(the 3-page VMO) = ", ours::handle_close(three), Status::Ok);
    if (ok) {
        auto _ = ort::debug_line("shell: vma_unmap checks passed");
    } else {
        auto _ = ort::debug_line("shell: vma_unmap checks FAILED");
    }
}

/// launch::start() reads a program only out of a VMO whose handle carries
/// Execute. Before it reads a byte, it maps the whole file into this process
/// with PermRead and PermExecute, and vma_map refuses that with AccessDenied
/// when the handle lacks Execute (component-framework design, D14 and I6):
///   - a page of zeros without Execute, which the judge (check_program())
///     would refuse with InvArgs, is AccessDenied: nothing judged it;
///   - fault_probe's file sliced out of the read-only BootFS, the bytes
///     run_probes() starts, is AccessDenied too: Execute is all it lacks;
///   - the same page of zeros made executable with vmex gets past the
///     mapping, and the judge refuses it with InvArgs.
/// Each refusal reports exactly one failed step, printed with its answer, and
/// consumes the bootstrap handle all the same: the peer sees PeerClosed.
///
/// Run only where this shell has a launcher of its own (dbg_1): start() needs
/// the vDSO and this process's region besides the file, which that launcher
/// holds, and the contrast needs vmex.
auto check_start_needs_execute(ours::Uandle const job, ours::Uandle const bootfs,
                               ours::Uandle const vmex, shell::Launcher const &library) -> void {
    using ours::Status;

    // The failed steps one start() reports, and the first one's words.
    struct Reported {
        u32 errors = 0;
        std::string_view first;
    };
    constexpr auto RECORD = [](void *const context, elfload::Report const &report) -> void {
        auto &reported = *static_cast<Reported *>(context);
        if (report.severity != elfload::Severity::Error) {
            return;
        }
        if (reported.errors == 0) {
            reported.first = report.what;
        }
        ++reported.errors;
    };

    // One start() of |file|: its answer against |want| on one line with the
    // step it reported, then that it reported one step, and that its
    // bootstrap handle is gone.
    auto const refuses = [&](std::string_view const what, ours::Uandle const file,
                             Status const want) -> bool {
        auto here = ours::Uandle::Invalid;
        auto there = ours::Uandle::Invalid;
        if (!expect("shell: channel_create = ",
                    ours::channel_create(ours::ChannelCreateOptions::None, &here, &there),
                    Status::Ok)) {
            return false;
        }
        Reported reported;
        auto const started = launch::start({
            .job = job,
            .name = "start_needs_execute",
            .file = file,
            .vma = library.vma,
            .vdso = library.vdso,
            .vdso_file = library.vdso_file,
            .bootstrap = there,
            .diagnostics = {.report = RECORD, .context = &reported},
        });
        auto got = Status::Ok;
        if (started.is_ok()) {
            // Started after all: taken down again, and the check fails below.
            auto _ = ours::handle_close(started.unwrap().process);
            auto _ = ours::handle_close(started.unwrap().root_vma);
        } else {
            got = started.unwrap_err();
        }
        auto const got_name = std::string_view{ours::to_string(got)};
        auto ok = got == want;
        if (ok) {
            auto _ = ort::debug_line(
                {"shell: start(", what, ") = ", got_name, ", at \"", reported.first, "\""});
        } else {
            auto _ = ort::debug_line({"shell: start(", what, ") = ", got_name, " (expected ",
                                      std::string_view{ours::to_string(want)}, "), at \"",
                                      reported.first, "\""});
        }
        if (reported.errors != 1) {
            char count[10];
            auto _ = ort::debug_line({"shell: start(", what, ") reported ",
                                      ort::decimal(reported.errors, count),
                                      " failed steps (expected 1)"});
            ok = false;
        }
        // A poll: start() has returned, so the bootstrap handle is closed or
        // never will be.
        constexpr ours::Instant NOW = 0;
        auto seen = ours::Signals::None;
        ok &=
            expect("shell: wait_one(the bootstrap's peer, PeerClosed) = ",
                   ours::object_wait_one(here, ours::Signals::PeerClosed, NOW, &seen), Status::Ok);
        ok &= expect("shell: close(the bootstrap's peer) = ", ours::handle_close(here), Status::Ok);
        return ok;
    };

    auto ok = true;
    constexpr auto PAGE = usize{4096};
    auto zeros = ours::Uandle::Invalid;
    if (!expect("shell: vmo_create(1 page) = ",
                ours::vmo_create(PAGE, ours::VmoOptions::None, &zeros), Status::Ok)) {
        auto _ = ort::debug_line("shell: start() Execute checks FAILED");
        return;
    }
    ok &= refuses("a page of zeros, no Execute", zeros, Status::AccessDenied);

    // fault_probe, out of the read-only BootFS: a program start() runs when
    // it comes from the executable copy instead.
    auto const view = bootfs::View::from({library.bootfs_file.data(), library.bootfs_file.size()});
    auto entry = ustl::Option<bootfs::Entry>{ustl::NONE};
    if (view.is_ok()) {
        if (auto const found = view.unwrap().find(user_programs::FAULT_PROBE); found.is_ok()) {
            entry = found.unwrap();
        }
    }
    if (entry.is_some()) {
        auto probe = ours::Uandle::Invalid;
        if (expect("shell: vmo_create_child(the read-only BootFS, Slice, fault_probe) = ",
                   ours::vmo_create_child(bootfs, ours::VmoChildOptions::Slice,
                                          entry.value().data_offset, entry.value().data_size,
                                          &probe),
                   Status::Ok)) {
            ok &= refuses("fault_probe, no Execute", probe, Status::AccessDenied);
            ok &= expect("shell: close(the fault_probe slice) = ", ours::handle_close(probe),
                         Status::Ok);
        } else {
            ok = false;
        }
    } else {
        auto _ = ort::debug_line("shell: fault_probe is not in the BootFS");
        ok = false;
    }

    // The zeros made executable: vmo_replace_as_executable consumes the
    // handle it is given, so it is given a duplicate, and the original is
    // closed below either way. The original stays writable meanwhile, so this
    // is the very VMO D14 (b) is safe from only because, on the dev image,
    // one process holds vmex and makes nothing but BootFS files executable
    // (I6); here this shell holds vmex itself.
    auto copy = ours::Uandle::Invalid;
    auto executable = ours::Uandle::Invalid;
    if (expect("shell: handle_duplicate(the zeros) = ",
               ours::handle_duplicate(zeros, ours::Rights::SameRights, &copy), Status::Ok) &&
        expect("shell: vmo_replace_as_executable(the zeros) = ",
               ours::vmo_replace_as_executable(copy, vmex, &executable), Status::Ok)) {
        ok &= refuses("a page of zeros, with Execute", executable, Status::InvArgs);
        ok &= expect("shell: close(the executable zeros) = ", ours::handle_close(executable),
                     Status::Ok);
    } else {
        ok = false;
    }
    ok &= expect("shell: close(the zeros) = ", ours::handle_close(zeros), Status::Ok);

    if (ok) {
        auto _ = ort::debug_line("shell: start() Execute checks passed");
    } else {
        auto _ = ort::debug_line("shell: start() Execute checks FAILED");
    }
}

/// The order in which check_loop()'s Waiters ran, one name per call.
struct LoopLog {
    std::array<char, 16> names{};
    usize size = 0;

    auto push(char const name) -> void {
        if (size < names.size()) {
            names[size] = name;
            ++size;
        }
    }

    [[nodiscard]]
    auto view() const -> std::string_view {
        return {names.data(), size};
    }
};

/// One slot's Waiter in check_loop(), shaped like a connection: it serves the
/// channel end |handle|. on_signals() reads one message and answers Rearm,
/// however many more are queued (I4); once nothing is readable, which here
/// means the peer has gone, it answers Close and leaves the closing to the
/// Loop (I3). When |suspend_next| is set, its next call reads the message and
/// answers Suspend instead, once: a connection that took a request whose
/// reply is not ready.
struct LoopClient final: loop::Waiter {
    LoopClient(char const client_name, ours::Uandle const client_handle, LoopLog *const client_log)
        : name{client_name}, handle{client_handle}, log{client_log} {}

    char name;
    ours::Uandle handle;
    LoopLog *log;
    bool suspend_next = false;

    auto on_signals(ours::Signals const observed) -> loop::Action override {
        log->push(name);
        if ((observed & ours::Signals::Readable) == ours::Signals::None) {
            return loop::Action::Close;
        }
        std::array<char, 8> message{};
        u32 actual = 0;
        if (ours::channel_read(handle, ours::ChannelReadOptions::None, message.data(), nullptr,
                               static_cast<u32>(message.size()), 0, &actual,
                               nullptr) != ours::Status::Ok) {
            return loop::Action::Close;
        }
        if (suspend_next) {
            suspend_next = false;
            return loop::Action::Suspend;
        }
        return loop::Action::Rearm;
    }
};
} // namespace

namespace shell::tests {
auto check_loop() -> void {
    using loop::Key;
    using ours::Signals;
    using ours::Status;
    using ours::Uandle;

    constexpr auto MESSAGES = Signals::Readable | Signals::PeerClosed;
    constexpr auto SLOTS = u32{4};
    constexpr std::string_view MESSAGE{"m"};

    auto ok = true;
    auto port = Uandle::Invalid;
    if (!expect("shell: loop port_create = ",
                ours::port_create(ours::PortCreateOptions::None, &port), Status::Ok)) {
        auto _ = ort::debug_line("shell: loop checks FAILED");
        return;
    }
    // A duplicate that can only queue: what forges the packets of step 4. The
    // Loop never hands its own port out.
    auto forger = Uandle::Invalid;
    ok &= expect("shell: loop handle_duplicate(port, Write) = ",
                 ours::handle_duplicate(port, ours::Rights::Write, &forger), Status::Ok);

    auto const channel = [&](Uandle &near, Uandle &far) -> void {
        ok &=
            expect("shell: loop channel_create = ",
                   ours::channel_create(ours::ChannelCreateOptions::None, &near, &far), Status::Ok);
    };
    auto const send = [&](Uandle const end, u32 const count) -> void {
        for (auto sent = u32{0}; sent < count; ++sent) {
            ok &= ours::channel_write(end, ours::ChannelWriteOptions::None, MESSAGE.data(),
                                      static_cast<u32>(MESSAGE.size()), nullptr, 0) == Status::Ok;
        }
    };
    auto const served = [&](LoopLog const &log, std::string_view const want) -> void {
        if (log.view() != want) {
            ok = false;
            auto _ = ort::debug_line(
                {"shell: loop served \"", log.view(), "\" (expected \"", want, "\")"});
        }
    };

    std::array<loop::Slot, SLOTS> slots{};
    auto log = LoopLog{};
    auto a0 = Uandle::Invalid;
    auto a1 = Uandle::Invalid;
    auto b0 = Uandle::Invalid;
    auto b1 = Uandle::Invalid;
    auto c0 = Uandle::Invalid;
    auto c1 = Uandle::Invalid;
    auto d0 = Uandle::Invalid;
    auto d1 = Uandle::Invalid;
    auto e0 = Uandle::Invalid;
    auto e1 = Uandle::Invalid;
    channel(a0, a1);
    channel(b0, b1);
    channel(c0, c1);
    channel(d0, d1);
    channel(e0, e1);
    auto a = LoopClient{'a', a1, &log};
    auto b = LoopClient{'b', b1, &log};
    auto c = LoopClient{'c', c1, &log};
    auto e = LoopClient{'e', e1, &log};
    {
        auto dispatch = loop::Loop{port, slots};
        // The Loop owns what add() accepted; what it refused is closed here.
        auto const add = [&](LoopClient &client, Key const want) -> void {
            auto const added = dispatch.add(client.handle, MESSAGES, client);
            if (added.is_err()) {
                ok = false;
                report("shell: loop add = ", added.unwrap_err());
                ok &= ours::handle_close(client.handle) == Status::Ok;
                return;
            }
            ok &= added.unwrap() == want;
        };

        // 1. Two channels, a with three messages and b with one. Each
        //    on_signals() reads one and is re-armed, behind the other's packet.
        add(a, Key{0, 0});
        add(b, Key{1, 0});
        send(a0, 3);
        send(b0, 1);
        ok &= expect("shell: loop run(two channels) = ", dispatch.run(0), Status::TimeOut);
        served(log, "abaa");

        // 2. b's peer goes: its Waiter answers Close, and the Loop closes
        //    b1, so that it no longer names anything here.
        ok &= ours::handle_close(b0) == Status::Ok;
        ok &= expect("shell: loop run(b's peer closed) = ", dispatch.run(0), Status::TimeOut);
        served(log, "abaab");
        ok &= expect("shell: loop close(b1) after the Loop closed it = ", ours::handle_close(b1),
                     Status::BadHandle);

        // 3. Slot 1 is taken again, in its second life.
        add(c, Key{1, 1});

        // 4. Packets that name no slot in use: slot 1's first life, a free
        //    slot, an index past the table, and a's key on a User packet.
        //    None may reach a Waiter. c's packet, queued behind them, must.
        send(d0, 1);
        for (auto const stale: {Key{1, 0}, Key{2, 0}, Key{SLOTS, 0}}) {
            ok &= expect("shell: loop wait_async(forged key) = ",
                         ours::object_wait_async(d1, forger, stale.raw(), Signals::Readable,
                                                 ours::WaitAsyncOptions::None),
                         Status::Ok);
        }
        auto user = ours::PortPacket{};
        user.key = Key{0, 0}.raw();
        ok &= expect("shell: loop port_queue(a's key) = ", ours::port_queue(forger, &user),
                     Status::Ok);
        send(c0, 1);
        ok &=
            expect("shell: loop run(forged packets, then c) = ", dispatch.run(0), Status::TimeOut);
        served(log, "abaabc");

        // 5. A slot that suspends. e takes a message and answers Suspend, so
        //    it stays in use with nothing armed; the message that arrives
        //    next is not delivered. resume() arms it, and then, and only then,
        //    the message is delivered. A second resume() finds the slot armed.
        e.suspend_next = true;
        add(e, Key{2, 0});
        send(e0, 1);
        ok &= expect("shell: loop run(e suspends) = ", dispatch.run(0), Status::TimeOut);
        served(log, "abaabce");
        send(e0, 1);
        ok &= expect("shell: loop run(e's message while suspended) = ", dispatch.run(0),
                     Status::TimeOut);
        served(log, "abaabce");
        ok &= expect("shell: loop resume(e) = ", dispatch.resume(Key{2, 0}), Status::Ok);
        ok &=
            expect("shell: loop resume(e again) = ", dispatch.resume(Key{2, 0}), Status::BadState);
        ok &= expect("shell: loop run(e resumed) = ", dispatch.run(0), Status::TimeOut);
        served(log, "abaabcee");

        // The Loop closes a1, c1, e1 and the port as it goes.
    }
    for (auto const handle: {a0, c0, d0, d1, e0, forger}) {
        ok &= ours::handle_close(handle) == Status::Ok;
    }

    if (ok) {
        auto _ = ort::debug_line("shell: loop checks passed");
    } else {
        auto _ = ort::debug_line("shell: loop checks FAILED");
    }
}

/// sampler_create, _start, _stop and _read, and the session's lifetime.
///
/// What needs no SAMPLING resource runs in every image: options are checked
/// before the resource, a resource that names nothing is BadHandle, and the
/// three calls on a session refuse a handle that is not a sampler. VMEX is
/// the wrong slot (OutOfRange) where this shell holds it. What needs the
/// resource runs where userboot started this shell:
///   - each refusal of a configuration;
///   - one session at a time: AlreadyExists while any handle to it is open,
///     a duplicate included, and a create succeeds once the last is closed;
///   - start, stop and read in their states: stop before start, start
///     twice, read while running and stop twice are BadState;
///   - a read: the size query, BufferTooSmall with the size, then the dump,
///     whose header says what follows, with at least one sample from a loop
///     of system calls while the session ran;
///   - closing the last handle while the session runs stops it, so a new
///     create succeeds.
/// A kernel-only session while this shell spins in user code, about twenty
/// million additions, many ticks long: the dump's header counts the ticks
/// that matched no target (num_unmatched), and some must have.
auto check_unmatched_ticks(ours::Uandle const sampling) -> bool {
    using ours::SamplerConfig;
    using ours::SamplerTargets;
    using ours::Status;

    auto const kernel_only = SamplerConfig{
        .period = 1'000'000,
        .buffer_size = 64 * sizeof(ours::SamplerRecord),
        .targets = SamplerTargets::Kernel,
        .reserved = 0,
    };
    auto handle = ours::Uandle::Invalid;
    if (!expect("shell: sampler_create(kernel only) = ",
                ours::sampler_create(sampling, 0, &kernel_only, &handle), Status::Ok)) {
        return false;
    }
    auto ok =
        expect("shell: sampler_start(kernel only) = ", ours::sampler_start(handle), Status::Ok);
    static u64 volatile sink = 0;
    for (u64 i = 0; i < 20'000'000; ++i) {
        sink = sink + i;
    }
    ok &= expect("shell: sampler_stop(kernel only) = ", ours::sampler_stop(handle), Status::Ok);
    ours::SamplerDumpHeader header{};
    auto actual = usize{0};
    static u8 dump[sizeof(ours::SamplerDumpHeader) + 16 * 64 * sizeof(ours::SamplerRecord)];
    ok &= expect("shell: sampler_read(kernel only) = ",
                 ours::sampler_read(handle, dump, sizeof(dump), &actual), Status::Ok);
    std::memcpy(&header, dump, sizeof(header));
    char digits[10];
    auto _ = ort::debug_line({"shell: kernel-only session counted ",
                              ort::decimal(static_cast<u32>(header.num_unmatched), digits),
                              " unmatched ticks"});
    ok &= header.num_unmatched > 0;
    auto _ = ours::handle_close(handle);
    return ok;
}

/// The stop barrier, the restart and the release, raced against the writers:
/// many short runs at the shortest period, each with this thread making
/// system calls and spinning in user code, so that the CPUs' timers are
/// writing kernel records and this thread its user ones when the run ends.
/// Each session runs twice, so that its second start has to empty what the
/// first run left. Most runs end with a stop and a read; the second run of
/// every other session ends with closing the running session, which stops it
/// and gives up the claim under the writers.
///
/// Each read is checked record by record: the magic, a depth of 1 to
/// SAMPLER_MAX_STACK_DEPTH, a CPU the header has a buffer for, no flag
/// beyond FromUser and Truncated, a process for every user record, and the
/// header's count matching what was copied. A writer that finished after the
/// stop's barrier, or a read under one, leaves a record that fails these.
/// And the timestamps of each run all lie after every one of the run before,
/// so a record left from an earlier run -- a buffer the restart did not empty
/// -- shows up too.
///
/// It can show a race, not prove there is none: the argument for the
/// barrier is in docs/changes/261007/thread-sampler.md. A stop meets a
/// writer mid-write only while the buffers have room: once one is full, a
/// writer advances the cursor and writes no slot. So the runs are short and
/// the buffers deep, and the summary counts the runs whose stop found room in
/// every buffer (no record dropped). That needs timers that fire at their own
/// deadlines, as the dev images boot (kernel.time.oneshot); under the
/// periodic tick a run takes at most one sample per CPU, and what is left is
/// a record from an earlier run, a malformed record, and closing a running
/// session.
auto check_sampler_stress(ours::Uandle const sampling) -> bool {
    using ours::SamplerConfig;
    using ours::SamplerRecordFlags;
    using ours::SamplerTargets;
    using ours::Status;

    constexpr auto SESSIONS = 16;
    constexpr auto RUNS_PER_SESSION = 2;
    constexpr auto RECORDS_PER_CPU = u64{64};
    constexpr auto MAX_CPUS = usize{16};
    constexpr auto HEADER_SIZE = sizeof(ours::SamplerDumpHeader);
    constexpr auto RECORD_SIZE = sizeof(ours::SamplerRecord);
    static u8 DUMP[HEADER_SIZE + MAX_CPUS * RECORDS_PER_CPU * RECORD_SIZE];

    auto const config = SamplerConfig{
        .period = ours::SAMPLER_MIN_PERIOD,
        .buffer_size = RECORDS_PER_CPU * RECORD_SIZE,
        .targets = SamplerTargets::Kernel | SamplerTargets::User,
        .reserved = 0,
    };
    constexpr auto KNOWN_FLAGS = SamplerRecordFlags::FromUser | SamplerRecordFlags::Truncated;

    auto ok = true;
    u32 runs = 0;
    u32 stopped_with_room = 0;
    u64 checked = 0;
    u64 previous_latest = 0;
    for (auto session = 0; session < SESSIONS && ok; ++session) {
        auto handle = ours::Uandle::Invalid;
        if (auto const status = ours::sampler_create(sampling, 0, &config, &handle);
            status != Status::Ok) {
            report("shell: sampler stress: sampler_create = ", status);
            return false;
        }
        auto closed = false;
        for (auto run = 0; run < RUNS_PER_SESSION && ok; ++run) {
            if (auto const status = ours::sampler_start(handle); status != Status::Ok) {
                report("shell: sampler stress: sampler_start = ", status);
                auto _ = ours::handle_close(handle);
                return false;
            }
            ++runs;
            // Kernel time and user time both, so that both kinds of writer
            // are busy when the run ends; short enough that a CPU sampled
            // every 10 µs has not filled its RECORDS_PER_CPU by then.
            static u64 volatile SINK = 0;
            for (auto i = 0; i < 20; ++i) {
                auto vmo = ours::Uandle::Invalid;
                if (ours::vmo_create(4096, ours::VmoOptions::None, &vmo) == Status::Ok) {
                    auto _ = ours::handle_close(vmo);
                }
                for (u64 j = 0; j < 2'000; ++j) {
                    SINK = SINK + j;
                }
            }
            if (session % 2 == 1 && run == RUNS_PER_SESSION - 1) {
                // Closing the running session: the release stops it under
                // the writers, and the next create finds the claim given up.
                ok &= ours::handle_close(handle) == Status::Ok;
                closed = true;
                break;
            }
            auto actual = usize{0};
            auto const stopped = ours::sampler_stop(handle);
            auto const read = ours::sampler_read(handle, DUMP, sizeof(DUMP), &actual);
            if (stopped != Status::Ok || read != Status::Ok) {
                report("shell: sampler stress: sampler_stop = ", stopped);
                report("shell: sampler stress: sampler_read = ", read);
                auto _ = ours::handle_close(handle);
                return false;
            }

            ours::SamplerDumpHeader header{};
            std::memcpy(&header, DUMP, sizeof(header));
            ok &= header.magic == ours::SAMPLER_DUMP_MAGIC && header.record_size == RECORD_SIZE &&
                  actual == HEADER_SIZE + usize{header.num_records} * RECORD_SIZE;
            if (header.num_dropped == 0) {
                ++stopped_with_room;
            }
            u64 latest = previous_latest;
            for (u32 i = 0; i < header.num_records && ok; ++i) {
                ours::SamplerRecord record{};
                std::memcpy(&record, DUMP + HEADER_SIZE + usize{i} * RECORD_SIZE, sizeof(record));
                auto const from_user = !!(record.flags & SamplerRecordFlags::FromUser);
                auto const whole =
                    record.magic == ours::SAMPLER_RECORD_MAGIC && record.depth >= 1 &&
                    record.depth <= ours::SAMPLER_MAX_STACK_DEPTH && record.cpu < header.num_cpus &&
                    !(record.flags & ~KNOWN_FLAGS) && (!from_user || record.pid != 0) &&
                    record.timestamp > previous_latest;
                if (!whole) {
                    char index[10];
                    char at[10];
                    auto _ = ort::debug_line(
                        {"shell: sampler stress: run ", ort::decimal(runs - 1, at), " record ",
                         ort::decimal(i, index), " is not a whole record of this run"});
                    ok = false;
                }
                latest = std::max(latest, record.timestamp);
                ++checked;
            }
            previous_latest = latest;
        }
        if (!closed) {
            auto _ = ours::handle_close(handle);
        }
    }

    char run_count[10];
    char session_count[10];
    char records[10];
    char with_room[10];
    auto _ = ort::debug_line({"shell: sampler stress: ", ort::decimal(runs, run_count), " runs in ",
                              ort::decimal(static_cast<u32>(SESSIONS), session_count),
                              " sessions, ", ort::decimal(static_cast<u32>(checked), records),
                              " records checked, ", ort::decimal(stopped_with_room, with_room),
                              " stops with room in every buffer"});
    return ok;
}

auto check_sampler(ours::Uandle const sampling, ours::Uandle const vmex) -> void {
    using ours::SamplerConfig;
    using ours::SamplerTargets;
    using ours::Status;

    constexpr auto HEADER_SIZE = sizeof(ours::SamplerDumpHeader);
    constexpr auto RECORD_SIZE = sizeof(ours::SamplerRecord);
    constexpr auto RECORDS_PER_CPU = u64{8};

    auto ok = true;
    auto handle = ours::Uandle::Invalid;
    auto const good = SamplerConfig{
        .period = 1'000'000,
        .buffer_size = RECORDS_PER_CPU * RECORD_SIZE,
        .targets = SamplerTargets::Kernel | SamplerTargets::User,
        .reserved = 0,
    };

    ok &= expect("shell: sampler_create(options = 1) = ",
                 ours::sampler_create(sampling, 1, &good, &handle), Status::InvArgs);
    ok &= expect("shell: sampler_create(no resource) = ",
                 ours::sampler_create(ours::Uandle::Invalid, 0, &good, &handle), Status::BadHandle);
    ours::Uandle a = ours::Uandle::Invalid;
    ours::Uandle b = ours::Uandle::Invalid;
    if (expect("shell: channel_create = ",
               ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok)) {
        ok &= expect("shell: sampler_start(a channel) = ", ours::sampler_start(a), Status::BadType);
        ok &= expect("shell: sampler_stop(a channel) = ", ours::sampler_stop(a), Status::BadType);
        usize actual = 0;
        ok &=
            expect("shell: sampler_read(a channel) = ", ours::sampler_read(a, nullptr, 0, &actual),
                   Status::BadType);
        auto _ = ours::handle_close(a);
        auto _ = ours::handle_close(b);
    } else {
        ok = false;
    }
    if (vmex != ours::Uandle::Invalid) {
        ok &=
            expect("shell: sampler_create(vmex) = ", ours::sampler_create(vmex, 0, &good, &handle),
                   Status::OutOfRange);
    }

    if (sampling == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: sampler sessions skipped (no SAMPLING resource)");
    } else {
        auto const refused = [&](std::string_view const what, SamplerConfig const &config,
                                 Status const want) {
            return expect(what, ours::sampler_create(sampling, 0, &config, &handle), want);
        };
        auto config = good;
        config.reserved = 1;
        ok &= refused("shell: sampler_create(reserved = 1) = ", config, Status::InvArgs);
        config = good;
        config.targets = SamplerTargets::None;
        ok &= refused("shell: sampler_create(no target) = ", config, Status::InvArgs);
        config.targets = static_cast<SamplerTargets>(0x4);
        ok &= refused("shell: sampler_create(an unknown target) = ", config, Status::InvArgs);
        config = good;
        config.period = ours::SAMPLER_MIN_PERIOD - 1;
        ok &= refused("shell: sampler_create(period too short) = ", config, Status::InvArgs);
        config.period = ours::SAMPLER_MAX_PERIOD + 1;
        ok &= refused("shell: sampler_create(period too long) = ", config, Status::InvArgs);
        config = good;
        config.buffer_size = RECORD_SIZE - 1;
        ok &= refused("shell: sampler_create(no whole record) = ", config, Status::InvArgs);
        config.buffer_size = ours::SAMPLER_MAX_BUFFER_SIZE + 1;
        ok &= refused("shell: sampler_create(buffer too large) = ", config, Status::InvArgs);

        // One session at a time, for as long as any handle to it is open.
        auto dup = ours::Uandle::Invalid;
        auto second = ours::Uandle::Invalid;
        ok &= expect("shell: sampler_create = ", ours::sampler_create(sampling, 0, &good, &handle),
                     Status::Ok);
        ok &= expect("shell: sampler_create(a second) = ",
                     ours::sampler_create(sampling, 0, &good, &second), Status::AlreadyExists);
        ok &= expect("shell: handle_duplicate(the sampler) = ",
                     ours::handle_duplicate(handle, ours::Rights::SameRights, &dup), Status::Ok);
        ok &= expect("shell: close(the sampler) = ", ours::handle_close(handle), Status::Ok);
        ok &= expect("shell: sampler_create(its duplicate open) = ",
                     ours::sampler_create(sampling, 0, &good, &second), Status::AlreadyExists);
        handle = dup;

        // The handle's rights gate the calls, before the session's state is
        // looked at: Write to start and stop, Read to read.
        auto blind = ours::Uandle::Invalid;
        ok &= expect("shell: handle_duplicate(the sampler, without Read and Write) = ",
                     ours::handle_duplicate(handle,
                                            ours::Rights::Transfer | ours::Rights::Duplicate |
                                                ours::Rights::Inspect,
                                            &blind),
                     Status::Ok);
        ok &= expect("shell: sampler_start(without Write) = ", ours::sampler_start(blind),
                     Status::AccessDenied);
        ok &= expect("shell: sampler_stop(without Write) = ", ours::sampler_stop(blind),
                     Status::AccessDenied);
        usize unread = 0;
        ok &= expect("shell: sampler_read(without Read) = ",
                     ours::sampler_read(blind, nullptr, 0, &unread), Status::AccessDenied);
        ok &= expect("shell: close(the copy without Read and Write) = ", ours::handle_close(blind),
                     Status::Ok);

        // Start, stop and read in their states.
        usize actual = 0;
        ok &= expect("shell: sampler_stop(before start) = ", ours::sampler_stop(handle),
                     Status::BadState);
        ok &= expect("shell: sampler_start = ", ours::sampler_start(handle), Status::Ok);
        ok &= expect("shell: sampler_start(running) = ", ours::sampler_start(handle),
                     Status::BadState);
        ok &= expect("shell: sampler_read(running) = ",
                     ours::sampler_read(handle, nullptr, 0, &actual), Status::BadState);
        // Time to sample, with no call to sleep on: a loop of system calls,
        // so that ticks land in kernel code as well as in this loop.
        // 5000 took about 40 ms on dbg (2026-10-05), some four ticks on
        // each CPU: enough that a run with no sample at all means a fault,
        // not bad luck.
        for (auto i = 0; i < 5'000; ++i) {
            auto vmo = ours::Uandle::Invalid;
            if (ours::vmo_create(4096, ours::VmoOptions::None, &vmo) == Status::Ok) {
                auto _ = ours::handle_close(vmo);
            }
        }
        ok &= expect("shell: sampler_stop = ", ours::sampler_stop(handle), Status::Ok);
        ok &=
            expect("shell: sampler_stop(stopped) = ", ours::sampler_stop(handle), Status::BadState);

        // The read: the size, a buffer one byte short, then the dump.
        static u8 DUMP[HEADER_SIZE + 16 * RECORDS_PER_CPU * RECORD_SIZE];
        usize needed = 0;
        ok &= expect("shell: sampler_read(the size) = ",
                     ours::sampler_read(handle, nullptr, 0, &needed), Status::Ok);
        ok &= needed >= HEADER_SIZE && needed <= sizeof(DUMP) &&
              (needed - HEADER_SIZE) % RECORD_SIZE == 0;
        ok &= expect("shell: sampler_read(one byte short) = ",
                     ours::sampler_read(handle, DUMP, needed - 1, &actual), Status::BufferTooSmall);
        ok &= actual == needed;
        ok &= expect("shell: sampler_read = ",
                     ours::sampler_read(handle, DUMP, sizeof(DUMP), &actual), Status::Ok);
        ok &= actual == needed;
        ours::SamplerDumpHeader header{};
        std::memcpy(&header, DUMP, sizeof(header));
        auto const records = header.num_records;
        ok &= header.magic == ours::SAMPLER_DUMP_MAGIC;
        ok &= header.header_size == HEADER_SIZE && header.record_size == RECORD_SIZE;
        ok &= records == (needed - HEADER_SIZE) / RECORD_SIZE;
        ok &= records > 0;
        char digits[10];
        auto _ =
            ort::debug_line({"shell: sampler read ", ort::decimal(records, digits), " records"});

        // Closing the last handle while the session runs stops and ends it.
        ok &= expect("shell: sampler_start(again) = ", ours::sampler_start(handle), Status::Ok);
        ok &=
            expect("shell: close(the running sampler) = ", ours::handle_close(handle), Status::Ok);
        ok &= expect("shell: sampler_create(after the close) = ",
                     ours::sampler_create(sampling, 0, &good, &handle), Status::Ok);
        ok &= expect("shell: close(the new sampler) = ", ours::handle_close(handle), Status::Ok);

        // A kernel-only session over user arithmetic: the ticks that land in
        // this loop match no target, record nothing, and are counted.
        ok &= check_unmatched_ticks(sampling);

        // Many short runs, each ended under the writers.
        ok &= check_sampler_stress(sampling);
    }

    if (ok) {
        auto _ = ort::debug_line("shell: sampler checks passed");
    } else {
        auto _ = ort::debug_line("shell: sampler checks FAILED");
    }
}

/// What object_get_info(HandleBasic) answers for |handle|, or why it did not.
auto handle_basic(ours::Uandle const handle) -> ustl::Result<ours::InfoHandleBasic, ours::Status> {
    ours::InfoHandleBasic basic{};
    if (auto const status = ours::object_get_info(handle, ours::InfoTopic::HandleBasic, &basic,
                                                  sizeof(basic), nullptr, nullptr);
        status != ours::Status::Ok) {
        return ustl::err(status);
    }
    return ustl::ok(basic);
}

/// object_get_info(HandleBasic): each handle's own rights and its object's
/// koid and type, for a handle of any type and one without Inspect, and the
/// related koid of the three objects bound to another from birth -- a
/// process to its job, a thread to its process, a channel end to its peer,
/// which it keeps after the peer has closed. Also that this process's
/// ProcSelf has every right a process is created with.
auto check_handle_basic(ours::Uandle const job) -> void {
    using ours::ObjectType;
    using ours::Rights;
    using ours::Status;

    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: handle basic checks skipped (no job)");
        return;
    }

    auto ok = expect("shell: object_get_info(Uandle::Invalid, HandleBasic) = ",
                     status_of(handle_basic(ours::Uandle::Invalid)), Status::BadHandle);

    auto process = ours::Uandle::Invalid;
    auto region = ours::Uandle::Invalid;
    if (!expect("shell: process_create = ",
                ours::process_create(job, "basic", sizeof("basic") - 1,
                                     ours::ProcessCreateOptions::None, &process, &region),
                Status::Ok)) {
        auto _ = ort::debug_line("shell: handle basic checks FAILED");
        return;
    }
    auto thread = ours::Uandle::Invalid;
    ok &= expect("shell: thread_create = ",
                 ours::thread_create(process, "basic", sizeof("basic") - 1,
                                     ours::ThreadCreateOptions::None, &thread),
                 Status::Ok);
    auto a = ours::Uandle::Invalid;
    auto b = ours::Uandle::Invalid;
    ok &= expect("shell: channel_create = ",
                 ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok);
    auto blind = ours::Uandle::Invalid;
    constexpr auto WITHOUT_INSPECT = Rights::Transfer | Rights::Wait;
    ok &= expect("shell: handle_duplicate(a process, without Inspect) = ",
                 ours::handle_duplicate(process, WITHOUT_INSPECT, &blind), Status::Ok);

    auto const of_job = handle_basic(job);
    auto const of_self = handle_basic(ort::process_self());
    auto const of_process = handle_basic(process);
    auto const of_region = handle_basic(region);
    auto const of_thread = handle_basic(thread);
    auto const of_a = handle_basic(a);
    auto const of_b = handle_basic(b);
    auto const of_blind = handle_basic(blind);
    if (of_job.is_err() || of_self.is_err() || of_process.is_err() || of_region.is_err() ||
        of_thread.is_err() || of_a.is_err() || of_b.is_err() || of_blind.is_err()) {
        auto _ = ort::debug_line("shell: object_get_info(HandleBasic) refused a live handle");
        ok = false;
    } else {
        auto const check = [&ok](bool const holds, std::string_view const what) {
            if (!holds) {
                auto _ = ort::debug_line({"shell: HandleBasic: ", what});
                ok = false;
            }
        };
        check(of_job->type == ObjectType::Job && of_job->related_koid == 0,
              "the job is not a Job with no related koid");
        check(of_process->type == ObjectType::Process && of_process->related_koid == of_job->koid,
              "the process is not a Process related to its job");
        check(of_process->koid != of_job->koid && of_process->koid != 0,
              "the process's koid is not its own");
        check(of_region->type == ObjectType::Vma && of_region->related_koid == 0,
              "the root region is not a Vma with no related koid");
        check(of_thread->type == ObjectType::Thread && of_thread->related_koid == of_process->koid,
              "the thread is not a Thread related to its process");
        check(of_blind->koid == of_process->koid && of_blind->rights == WITHOUT_INSPECT,
              "the handle without Inspect does not name the process with its own rights");
        // Both process handles come straight from creation: process_create
        // here, and the starter's own, duplicated with SameRights, as ProcSelf.
        check(of_self->type == ObjectType::Process && of_self->rights == of_process->rights,
              "ProcSelf does not have the rights a process is created with");
        check(of_a->type == ObjectType::Channel && of_a->related_koid == of_b->koid &&
                  of_b->related_koid == of_a->koid,
              "the channel's ends are not each related to the other");
        // The peer's koid is fixed when the pair is made, so it outlives the
        // peer.
        auto _ = ours::handle_close(b);
        b = ours::Uandle::Invalid;
        auto const after = handle_basic(a);
        check(after.is_ok() && after->related_koid == of_b->koid,
              "a channel end does not keep its peer's koid after the peer closed");
    }
    for (auto const handle: {blind, a, b, thread, region, process}) {
        if (handle != ours::Uandle::Invalid) {
            auto _ = ours::handle_close(handle);
        }
    }
    auto _ = ort::debug_line(ok ? "shell: handle basic checks passed"
                                : "shell: handle basic checks FAILED");
}

/// This process's handle to itself (ort::process_self()), which its starter
/// handed over as ProcSelf -- userboot on dbg_1, the root's runner on dev --
/// and the runtime took before main: it names this process, which has
/// started and not ended, and no ProcSelf is left among the startup handles.
auto check_process_self() -> void {
    using ours::InfoProcessFlags;
    using ours::Status;

    auto ok = true;
    ours::InfoProcess info{};
    if (auto const status = ours::object_get_info(ort::process_self(), ours::InfoTopic::Process,
                                                  &info, sizeof(info), nullptr, nullptr);
        status != Status::Ok) {
        report("shell: object_get_info(process_self()) = ", status);
        ok = false;
    } else if (!(info.flags & InfoProcessFlags::Started) ||
               !!(info.flags & InfoProcessFlags::Exited)) {
        auto _ = ort::debug_line("shell: process_self() is not a running process");
        ok = false;
    }
    if (auto const left = ort::take_startup_handle(ours::HandleType::ProcSelf, 0);
        left != ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: ProcSelf was left among the startup handles");
        auto _ = ours::handle_close(left);
        ok = false;
    }
    if (ok) {
        auto _ = ort::debug_line("shell: process self checks passed");
    } else {
        auto _ = ort::debug_line("shell: process self checks FAILED");
    }
}

/// This program's two mappings among |maps|: the executable one that holds
/// |here|, an address of its code, and the one at offset 0 of the same VMO,
/// where its ELF header is, since the loader maps a program by segment. Null
/// for one that is not there.
struct ProgramMappings {
    ours::InfoMaps const *code;
    ours::InfoMaps const *header;
};

auto find_program_mappings(std::span<ours::InfoMaps const> const maps, u64 const here)
    -> ProgramMappings {
    auto found = ProgramMappings{nullptr, nullptr};
    for (auto const &entry: maps) {
        if (entry.type == ours::InfoMapsType::Mapping && here - entry.base < entry.size) {
            found.code = &entry;
        }
    }
    if (found.code == nullptr || found.code->u.mapping.vmo_koid == 0) {
        return found;
    }
    for (auto const &entry: maps) {
        if (entry.type == ours::InfoMapsType::Mapping &&
            entry.u.mapping.vmo_koid == found.code->u.mapping.vmo_koid &&
            entry.u.mapping.vmo_offset == 0) {
            found.header = &entry;
        }
    }
    return found;
}

/// object_get_info(ProcessMaps) on this process, through its handle to
/// itself: the address space comes first, at depth 0; this function's code
/// lies in a readable, executable, unwritable mapping of a VMO with a koid;
/// the same VMO is mapped at offset 0 too, where the ELF header is, since the
/// loader maps a program by segment; a buffer of one record is Ok, with
/// actual 1 and avail more; a handle without Inspect is AccessDenied, and one
/// that names no process is BadType.
auto check_process_maps() -> void {
    using ours::InfoMaps;
    using ours::InfoMapsType;
    using ours::InfoTopic;
    using ours::Rights;
    using ours::Status;
    using ours::VmOptions;

    static InfoMaps MAPS[128];
    auto const self = ort::process_self();
    usize actual = 0;
    usize avail = 0;
    auto ok = expect(
        "shell: object_get_info(self, ProcessMaps) = ",
        ours::object_get_info(self, InfoTopic::ProcessMaps, MAPS, sizeof(MAPS), &actual, &avail),
        Status::Ok);
    auto const check = [&ok](bool const holds, std::string_view const what) {
        if (!holds) {
            auto _ = ort::debug_line({"shell: ProcessMaps: ", what});
            ok = false;
        }
    };
    if (ok) {
        check(actual >= 1 && actual == avail,
              "the records do not fit 128, or actual and avail differ");
        check(MAPS[0].type == InfoMapsType::Aspace && MAPS[0].depth == 0,
              "the first record is not the address space at depth 0");

        auto const found = find_program_mappings(std::span<InfoMaps const>{MAPS, actual},
                                                 reinterpret_cast<u64>(&check_process_maps));
        check(found.code != nullptr, "no mapping holds this function");
        if (found.code != nullptr) {
            auto const flags = found.code->u.mapping.mmu_flags;
            check(!!(flags & VmOptions::PermRead) && !!(flags & VmOptions::PermExecute) &&
                      !(flags & VmOptions::PermWrite),
                  "the code's mapping is not read and execute only");
            check(found.code->u.mapping.vmo_koid != 0, "the code's VMO has no koid");
            check(found.code->depth >= 2, "a mapping is not below the root region");
            check(found.header != nullptr, "the code's VMO is not mapped at offset 0 too");
        }
    }

    usize one_actual = 0;
    usize one_avail = 0;
    ok &= expect("shell: object_get_info(self, ProcessMaps, one record) = ",
                 ours::object_get_info(self, InfoTopic::ProcessMaps, MAPS, sizeof(InfoMaps),
                                       &one_actual, &one_avail),
                 Status::Ok);
    check(one_actual == 1 && one_avail == avail, "a short buffer does not give one record and all");

    auto blind = ours::Uandle::Invalid;
    ok &= expect("shell: handle_duplicate(self, without Inspect) = ",
                 ours::handle_duplicate(self, Rights::Transfer | Rights::Wait, &blind), Status::Ok);
    ok &= expect(
        "shell: object_get_info(without Inspect, ProcessMaps) = ",
        ours::object_get_info(blind, InfoTopic::ProcessMaps, MAPS, sizeof(MAPS), nullptr, nullptr),
        Status::AccessDenied);
    auto _ = ours::handle_close(blind);

    auto a = ours::Uandle::Invalid;
    auto b = ours::Uandle::Invalid;
    ok &= expect("shell: channel_create = ",
                 ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok);
    ok &= expect(
        "shell: object_get_info(a channel, ProcessMaps) = ",
        ours::object_get_info(a, InfoTopic::ProcessMaps, MAPS, sizeof(MAPS), nullptr, nullptr),
        Status::BadType);
    auto _ = ours::handle_close(a);
    auto _ = ours::handle_close(b);

    if (ok) {
        auto _ = ort::debug_line("shell: process maps checks passed");
    } else {
        auto _ = ort::debug_line("shell: process maps checks FAILED");
    }
}

/// process_read_memory on this process, through its handle to itself: the
/// mapping at offset 0 of this program's VMO starts with the ELF magic; this
/// function's own bytes read back as they lie in memory; a read is cut at
/// the end of its mapping; an address no mapping holds is NotFound; a size of
/// 0, or past MAX_PROCESS_READ_MEMORY, is InvArgs; a handle without Write is
/// AccessDenied, and one that names no process BadType.
auto check_process_read_memory() -> void {
    using ours::InfoMaps;
    using ours::InfoTopic;
    using ours::Rights;
    using ours::Status;

    static InfoMaps MAPS[128];
    auto const self = ort::process_self();
    usize count = 0;
    auto ok = expect(
        "shell: object_get_info(self, ProcessMaps) = ",
        ours::object_get_info(self, InfoTopic::ProcessMaps, MAPS, sizeof(MAPS), &count, nullptr),
        Status::Ok);
    auto const check = [&ok](bool const holds, std::string_view const what) {
        if (!holds) {
            auto _ = ort::debug_line({"shell: process_read_memory: ", what});
            ok = false;
        }
    };
    auto const here = reinterpret_cast<u64>(&check_process_read_memory);
    auto const found = find_program_mappings(std::span<InfoMaps const>{MAPS, count}, here);
    if (!ok || found.code == nullptr || found.header == nullptr) {
        auto _ = ort::debug_line("shell: process_read_memory checks FAILED (no program mappings)");
        return;
    }

    u8 bytes[64] = {};
    usize actual = 0;
    ok &= expect(
        "shell: process_read_memory(self, ELF header) = ",
        ours::process_read_memory(self, ours::VirtAddr{found.header->base}, bytes, 4, &actual),
        Status::Ok);
    check(actual == 4 && bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F',
          "the mapping at offset 0 does not start with the ELF magic");

    ok &= expect("shell: process_read_memory(self, this function) = ",
                 ours::process_read_memory(self, ours::VirtAddr{here}, bytes, 16, &actual),
                 Status::Ok);
    check(actual == 16 && std::memcmp(bytes, reinterpret_cast<void const *>(here), 16) == 0,
          "this function's bytes do not read back as they are");

    // Cut at the mapping's end, unless the next record continues the same
    // mapping (a second protection range of it).
    auto const end = found.code->base + found.code->size;
    auto continued = false;
    for (usize i = 0; i < count; ++i) {
        continued = continued || (MAPS[i].base == end &&
                                  MAPS[i].u.mapping.vmo_koid == found.code->u.mapping.vmo_koid);
    }
    ok &= expect(
        "shell: process_read_memory(self, across the code's end) = ",
        ours::process_read_memory(self, ours::VirtAddr{end - 16}, bytes, sizeof(bytes), &actual),
        Status::Ok);
    check(continued || actual == 16, "a read is not cut at its mapping's end");

    ok &= expect("shell: process_read_memory(self, address 0) = ",
                 ours::process_read_memory(self, ours::VirtAddr{0}, bytes, sizeof(bytes), &actual),
                 Status::NotFound);
    ok &= expect("shell: process_read_memory(self, 0 bytes) = ",
                 ours::process_read_memory(self, ours::VirtAddr{here}, bytes, 0, &actual),
                 Status::InvArgs);
    ok &= expect("shell: process_read_memory(self, past the limit) = ",
                 ours::process_read_memory(self, ours::VirtAddr{here}, bytes,
                                           ours::MAX_PROCESS_READ_MEMORY + 1, &actual),
                 Status::InvArgs);

    auto reader = ours::Uandle::Invalid;
    ok &=
        expect("shell: handle_duplicate(self, without Write) = ",
               ours::handle_duplicate(self, Rights::Transfer | Rights::Read, &reader), Status::Ok);
    ok &= expect("shell: process_read_memory(without Write) = ",
                 ours::process_read_memory(reader, ours::VirtAddr{here}, bytes, 16, &actual),
                 Status::AccessDenied);
    auto _ = ours::handle_close(reader);

    auto a = ours::Uandle::Invalid;
    auto b = ours::Uandle::Invalid;
    ok &= expect("shell: channel_create = ",
                 ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok);
    ok &= expect("shell: process_read_memory(a channel) = ",
                 ours::process_read_memory(a, ours::VirtAddr{here}, bytes, 16, &actual),
                 Status::BadType);
    auto _ = ours::handle_close(a);
    auto _ = ours::handle_close(b);

    if (ok) {
        auto _ = ort::debug_line("shell: process read memory checks passed");
    } else {
        auto _ = ort::debug_line("shell: process read memory checks FAILED");
    }
}

/// Whether |koid| is among the first |count| of |koids|.
auto lists(ours::KoId const *const koids, usize const count, ours::KoId const koid) -> bool {
    for (usize i = 0; i < count; ++i) {
        if (koids[i] == koid) {
            return true;
        }
    }
    return false;
}

/// object_get_info(JobProcesses) and object_get_child on the job this shell
/// holds: the job lists this shell; a short buffer is Ok; a process made and
/// still held is listed, and once its last handle is closed (it dies
/// unstarted) it is not; object_get_child by this shell's koid gives a handle
/// naming this shell; rights past the job handle's are AccessDenied, an
/// unknown koid NotFound; a job handle without Enumerate is AccessDenied for
/// both, and so is a channel, which grants none (the rights are checked
/// before the type); a process with no such thread is NotFound.
auto check_job_processes(ours::Uandle const job) -> void {
    using ours::InfoTopic;
    using ours::KoId;
    using ours::Rights;
    using ours::Status;

    if (job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: job process checks skipped (no job)");
        return;
    }
    auto const self = handle_basic(ort::process_self());
    if (self.is_err()) {
        auto _ = ort::debug_line("shell: job process checks FAILED (no koid of its own)");
        return;
    }
    auto const self_koid = self->koid;

    static KoId KOIDS[256];
    usize actual = 0;
    usize avail = 0;
    auto ok = expect(
        "shell: object_get_info(job, JobProcesses) = ",
        ours::object_get_info(job, InfoTopic::JobProcesses, KOIDS, sizeof(KOIDS), &actual, &avail),
        Status::Ok);
    auto const check = [&ok](bool const holds, std::string_view const what) {
        if (!holds) {
            auto _ = ort::debug_line({"shell: JobProcesses: ", what});
            ok = false;
        }
    };
    check(actual == avail && lists(KOIDS, actual, self_koid), "the job does not list this shell");

    usize one_actual = 0;
    usize one_avail = 0;
    ok &= expect("shell: object_get_info(job, JobProcesses, one koid) = ",
                 ours::object_get_info(job, InfoTopic::JobProcesses, KOIDS, sizeof(KoId),
                                       &one_actual, &one_avail),
                 Status::Ok);
    check(one_actual == 1 && one_avail >= 1, "a short buffer does not give one koid");

    // A process made and held is listed; closing its last handle kills it
    // unstarted, and a dead process is not.
    auto process = ours::Uandle::Invalid;
    auto region = ours::Uandle::Invalid;
    ok &= expect("shell: process_create(listed) = ",
                 ours::process_create(job, "listed", sizeof("listed") - 1,
                                      ours::ProcessCreateOptions::None, &process, &region),
                 Status::Ok);
    auto const made = handle_basic(process);
    if (made.is_ok()) {
        ok &= expect("shell: object_get_info(job, JobProcesses, with it) = ",
                     ours::object_get_info(job, InfoTopic::JobProcesses, KOIDS, sizeof(KOIDS),
                                           &actual, nullptr),
                     Status::Ok);
        check(lists(KOIDS, actual, made->koid), "a process made and held is not listed");
        auto _ = ours::handle_close(region);
        auto _ = ours::handle_close(process);
        ok &= expect("shell: object_get_info(job, JobProcesses, after it) = ",
                     ours::object_get_info(job, InfoTopic::JobProcesses, KOIDS, sizeof(KOIDS),
                                           &actual, nullptr),
                     Status::Ok);
        check(!lists(KOIDS, actual, made->koid), "a process that died unstarted is still listed");
    } else {
        check(false, "the process made has no koid");
    }

    auto child = ours::Uandle::Invalid;
    ok &= expect("shell: object_get_child(job, this shell) = ",
                 ours::object_get_child(job, self_koid, Rights::SameRights, &child), Status::Ok);
    auto const of_child = handle_basic(child);
    check(of_child.is_ok() && of_child->koid == self_koid,
          "object_get_child does not name this shell");
    auto _ = ours::handle_close(child);
    ok &= expect("shell: object_get_child(job, this shell, past the job's rights) = ",
                 ours::object_get_child(job, self_koid, Rights::Execute, &child),
                 Status::AccessDenied);
    ok &=
        expect("shell: object_get_child(job, no such koid) = ",
               ours::object_get_child(job, ~KoId{0}, Rights::SameRights, &child), Status::NotFound);

    auto blind = ours::Uandle::Invalid;
    ok &=
        expect("shell: handle_duplicate(job, without Enumerate) = ",
               ours::handle_duplicate(job, Rights::Transfer | Rights::Inspect, &blind), Status::Ok);
    ok &= expect("shell: object_get_info(without Enumerate, JobProcesses) = ",
                 ours::object_get_info(blind, InfoTopic::JobProcesses, KOIDS, sizeof(KOIDS),
                                       nullptr, nullptr),
                 Status::AccessDenied);
    ok &= expect("shell: object_get_child(without Enumerate) = ",
                 ours::object_get_child(blind, self_koid, Rights::SameRights, &child),
                 Status::AccessDenied);
    auto _ = ours::handle_close(blind);

    auto a = ours::Uandle::Invalid;
    auto b = ours::Uandle::Invalid;
    ok &= expect("shell: channel_create = ",
                 ours::channel_create(ours::ChannelCreateOptions::None, &a, &b), Status::Ok);
    ok &= expect("shell: object_get_child(a channel) = ",
                 ours::object_get_child(a, self_koid, Rights::SameRights, &child),
                 Status::AccessDenied);
    auto _ = ours::handle_close(a);
    auto _ = ours::handle_close(b);
    ok &= expect("shell: object_get_child(this shell, no such thread) = ",
                 ours::object_get_child(ort::process_self(), self_koid, Rights::SameRights, &child),
                 Status::NotFound);

    if (ok) {
        auto _ = ort::debug_line("shell: job process checks passed");
    } else {
        auto _ = ort::debug_line("shell: job process checks FAILED");
    }
}

/// ours.kernel.RootJob, which the root serves in this shell's /svc on dev: a
/// Get answers a handle to the job this shell was handed too (the same
/// koid), with Enumerate, Read and Write, and processes can be listed
/// through it. dbg_1 has no root and no /svc: skipped.
auto check_root_job(bool const component, ours::Uandle const job) -> void {
    using ours::Rights;
    using ours::Status;

    if (!component || job == ours::Uandle::Invalid) {
        auto _ = ort::debug_line("shell: root job checks skipped (no namespace)");
        return;
    }
    auto const fetched = ours_resource::get(ours_resource::ROOT_JOB_PROTOCOL);
    auto ok = expect("shell: ours.kernel.RootJob Get = ", status_of(fetched), Status::Ok);
    if (fetched.is_ok()) {
        auto const root = fetched.unwrap();
        auto const of_root = handle_basic(root);
        auto const of_job = handle_basic(job);
        constexpr auto WANTED = Rights::Enumerate | Rights::Read | Rights::Write;
        if (of_root.is_err() || of_job.is_err() || of_root->type != ours::ObjectType::Job ||
            of_root->koid != of_job->koid || (of_root->rights & WANTED) != WANTED) {
            auto _ = ort::debug_line("shell: RootJob is not the root job with Enumerate, Read "
                                     "and Write");
            ok = false;
        }
        usize listed = 0;
        ok &= expect("shell: object_get_info(RootJob, JobProcesses) = ",
                     ours::object_get_info(root, ours::InfoTopic::JobProcesses, nullptr, 0, nullptr,
                                           &listed),
                     Status::Ok);
        if (listed == 0) {
            auto _ = ort::debug_line("shell: RootJob lists no process");
            ok = false;
        }
        auto _ = ours::handle_close(root);
    }
    if (ok) {
        auto _ = ort::debug_line("shell: root job checks passed");
    } else {
        auto _ = ort::debug_line("shell: root job checks FAILED");
    }
}

auto run(Inputs const &inputs) -> void {
    check_clock();
    check_process_self();
    check_process_maps();
    check_process_read_memory();
    check_job_processes(inputs.job);
    check_root_job(inputs.component, inputs.job);
    check_handle_basic(inputs.job);
    // The probes need fault_probe out of a raw BootFS and VMEX to start it
    // with: dbg_1 only, where userboot hands both over. On dev the root keeps
    // both, and its BootFS does not carry fault_probe.
    if (inputs.job != ours::Uandle::Invalid && inputs.bootfs != ours::Uandle::Invalid &&
        inputs.vmex != ours::Uandle::Invalid && inputs.launch_program != nullptr) {
        run_probes(inputs.job, inputs.launch_program, inputs.launcher_context);
        check_job_debugger(inputs.job, inputs.launch_program, inputs.launcher_context);
        check_task_kill_probes(inputs.job, inputs.launch_program, inputs.launcher_context);
    }
    // Before check_handle_close() too: it starts a probe in the job.
    check_ioports(inputs.job, inputs.ioport, inputs.irq, inputs.launch_program,
                  inputs.launcher_context, inputs.component);
    check_process_create(inputs.job);
    check_thread_create(inputs.job);
    check_process_start(inputs.job);
    check_thread_start(inputs.vma);
    check_task_suspend(inputs.vma);
    check_task_kill(inputs.job, inputs.vma);
    check_pager(inputs.vma);
    check_vdso_map(inputs.job, inputs.vdso);
    // Before check_handle_close(), which closes the job this program was
    // given, and the job is what makes the process for the sealed checks.
    check_vma_unmap(inputs.vma, inputs.job, inputs.vdso);
    // Also before check_handle_close(), for the same reason: each of these
    // starts processes in the job, or hands it to the Launcher to.
    if (inputs.library != nullptr) {
        check_start_needs_execute(inputs.job, inputs.bootfs, inputs.vmex, *inputs.library);
    }
    check_launcher_service(inputs.component, inputs.job, inputs.vmex);
    check_profile(inputs.component, inputs.job);
    check_spawn(inputs.component, inputs.job);
    check_vfs(inputs.component);
    check_handle_close(inputs.job);
    check_channel_loopback();
    check_handle_transfer();
    check_port();
    check_wait_async();
    check_port_observer_limit();
    check_loop();
    check_handle_quota();
    check_bad_pointers(ort::vdso_base());
    check_interrupt(inputs.irq, inputs.ioport, inputs.component);
    check_interrupt_port();
    check_interrupt_idle();
    check_sampler(inputs.sampling, inputs.vmex);
    check_vmo_read();
    check_vmo_write(inputs.bootfs);
    check_vmo_create();
    check_bootfs(inputs.bootfs, ort::vdso_base());
    check_handle_duplicate(inputs.bootfs);
    check_vmo_create_child(inputs.bootfs);
    check_vmo_get_size(inputs.bootfs);
    check_vmo_replace_as_executable(inputs.bootfs, inputs.vmex);
    check_resources(inputs.irq, inputs.ioport, inputs.vmex, inputs.component);
    check_vma_map(inputs.vma, inputs.bootfs, inputs.vmex);
    check_vma_allocate(inputs.vma);
    check_libc(static_cast<u8>(std::to_underlying(ort::vdso_base()) >> 20));
}
} // namespace shell::tests
