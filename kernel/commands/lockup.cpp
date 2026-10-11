/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `lockup <what>`: drive the lockup detector (//kernel/lib/lockup_detector)
// on purpose, so that its report can be tested the way it is used.
//
//   lockup status             the checker's state: the threshold, each
//                             CPU's heartbeat and its notes on the others,
//                             and the closest any CPU has come to being
//                             found stuck; before it, how each CPU's writes
//                             to the console went, the one stretch with
//                             interrupts off that every CPU goes through;
//                             nothing else happens
//   lockup nmi                send this CPU an NMI that nothing in the
//                             kernel asked for, as QEMU's monitor `nmi` or a
//                             front-panel button does: every CPU's state is
//                             printed once, and the machine panics
//   lockup spin <cpu> [<ms>]  turn interrupts off on CPU <cpu> and spin
//                             there, for <ms> milliseconds or, without it,
//                             until the detector finds the CPU stuck and
//                             reports, which ends the machine
//   lockup spin all           the same on every online CPU at once, for
//                             good: no CPU takes a tick, so none is left to
//                             find the others, and only an NMI from outside
//                             the machine gets a report -- the one the boot
//                             scripts send once the console has gone silent
//
// The report never comes in a healthy boot, and what it prints, and in what
// order, is the whole of what there is to check. `lockup status` is the one
// that reads rather than drives: after a healthy boot it says how far below
// the threshold every CPU stayed, which is the data the threshold rests on.
#include <ours/arch/mp.hpp>
#include <ours/cpu.hpp>
#include <ours/start.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>
#include <string_view>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <lockup_detector/lockup_detector.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>

namespace {
using namespace ours;

/// How long `lockup nmi` waits for its own NMI before it says it never came.
/// An NMI a CPU sends itself arrives before the next instruction; a second
/// is a bound for the case where it never does, nothing more.
constexpr auto NMI_WAIT = Seconds(1);

/// The most digits `lockup spin` reads in a number: a value up to
/// 999,999,999 cannot overflow on the way in.
constexpr auto MAX_DIGITS = usize{9};

/// `lockup nmi`. Returns only if the NMI was not sent or never arrived.
///
/// The NMI goes to this CPU through the same path the report itself uses
/// for the others, arch_mp_send_nmi(). Preemption stays off from asking
/// which CPU this is until the NMI is sent, so that the thread cannot move
/// and send it to a CPU it is no longer on. The NMI lands while the send
/// still has interrupts off around the ICR writes, so this thread's block
/// says "interrupts off" and points into the send.
auto dump_by_nmi() -> int {
    task::AutoPreemptDisabler same_cpu;
    auto const self = Cpu::id();
    auto const unsent = arch_mp_send_nmi(CpuMask::of(self));
    same_cpu.enable();
    if (unsent.any()) {
        log::error("lockup: could not send cpu{} an NMI: its ICR never went idle", self);
        return 1;
    }
    // Woken on time or early, the answer is the same: the NMI did not come.
    auto _ = task::Thread::Current::sleep_for(NMI_WAIT, task::Interruptible::No);
    log::error("lockup: cpu{} sent itself an NMI, and it never arrived", self);
    return 1;
}

/// How long the spinning thread spins: none for "until the report ends it".
using SpinLength = ustl::Option<Milliseconds>;

/// The spinning thread, pinned to its CPU: interrupts off there, then a busy
/// loop for |*arg|, or for good.
///
/// With interrupts off the CPU takes no tick, so its heartbeat stops while
/// the other CPUs' go on: what a CPU spinning on a spin lock looks like to
/// the detector, since every spin lock is waited for with interrupts off.
/// Only an NMI gets in, and the report's NMI parks the CPU right here, so
/// its block points into this loop with interrupts off.
///
/// The clock is read, not the tick: SteadyClock reads the clock source's
/// counter, which goes on with interrupts off.
auto spin_routine(void *const arg) -> i32 {
    auto const length = *static_cast<SpinLength const *>(arg);
    arch::AutoInterruptDisabler const no_tick;
    auto const start = time::SteadyClock::now();
    while (length.is_none() || time::SteadyClock::now() - start < length.value()) {
        arch::spin_hint();
    }
    return 0;
}

/// `lockup spin <cpu> [<ms>]`. Without <ms>, returns only if the spinning
/// thread could not be started.
///
/// The spinning thread is joined, so the command takes as long as the spin:
/// the shell's prompt coming back says the CPU has interrupts on again. The
/// join is also what lets the thread read |length| from this frame.
auto spin(CpuNum const cpu, SpinLength length) -> int {
    if (cpu >= MAX_CPU || !Cpu::is_online(cpu)) {
        log::error("lockup: cpu{} is not online", cpu);
        return 1;
    }
    auto *const thread =
        task::Thread::spawn("lockup-spin", spin_routine, &length, task::DEFAULT_PRIORITY);
    if (thread == nullptr) {
        log::error("lockup: could not start a thread to spin on cpu{}", cpu);
        return 1;
    }
    thread->set_cpu_affinity_hard(CpuMask::of(cpu));

    if (length.is_none()) {
        log::info("lockup: cpu{} spins with interrupts off until the lockup detector reports it",
                  cpu);
    } else {
        log::info("lockup: cpu{} spins with interrupts off for {} ms", cpu, length.value().count());
    }
    auto _ = thread->resume();

    auto retcode = i32{0};
    if (thread->join(&retcode, time::Deadline::infinite()) != Status::Ok) {
        log::error("lockup: the thread spinning on cpu{} could not be joined", cpu);
        return 1;
    }
    log::info("lockup: cpu{} has interrupts on again", cpu);
    return 0;
}

/// The threads of `lockup spin all`, indexed by the CPU each is pinned to;
/// null where none was made.
using SpinThreads = std::array<task::Thread *, MAX_CPU>;

/// Joins every thread in |threads|; false if one could not be joined.
auto join_all(SpinThreads const &threads) -> bool {
    auto joined = true;
    for (auto *const thread: threads) {
        if (thread != nullptr && thread->join(nullptr, time::Deadline::infinite()) != Status::Ok) {
            joined = false;
        }
    }
    return joined;
}

/// `lockup spin all`. Returns only if the spinning threads could not all be
/// made.
///
/// Every CPU spins with interrupts off at once, so none takes a tick and
/// none is left to check the others: the hang the detector cannot see from
/// inside. Only an NMI from outside reaches it, and the boot scripts send
/// QEMU's monitor `nmi` once the console has been silent for a while; this
/// is what tests that path.
///
/// Every thread is made and pinned before any is let go. Making one
/// allocates its stack, and an allocation may wait on the other CPUs, as a
/// TLB shootdown waits for each to answer its IPI. With some CPUs already
/// spinning, that wait would never end, and this CPU, waiting with
/// interrupts on, would go on ticking and find the others stuck: a hang of
/// some CPUs, which the detector reports by itself, not the one asked for.
/// Preemption stays off while they are let go, because the thread pinned to
/// this CPU would otherwise take it as soon as it is let go and never give
/// it back, and the threads after it would never start.
auto spin_all() -> int {
    auto length = SpinLength();
    auto threads = SpinThreads{};
    auto made = true;
    Cpu::online_mask().for_each([&length, &threads, &made](CpuNum const cpu) {
        auto *const thread =
            task::Thread::spawn("lockup-spin", spin_routine, &length, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            made = false;
            return;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        threads[cpu] = thread;
    });

    if (made) {
        log::info("lockup: every CPU spins with interrupts off; none is left to find the others, "
                  "so only an NMI from outside the machine reports them");
    } else {
        // The threads made so far are let go with nothing to spin for, so
        // that they end at once and can be reaped. They read the length
        // only once they run, after the resume below has published it.
        log::error("lockup: could not start a thread to spin on every CPU");
        length = SpinLength(Milliseconds(0));
    }
    {
        task::AutoPreemptDisabler const together;
        for (auto *const thread: threads) {
            if (thread != nullptr) {
                // A thread that is not Dead is always resumed, and these
                // have not run yet.
                auto _ = thread->resume();
            }
        }
    }

    if (!join_all(threads)) {
        log::error("lockup: a thread spinning for `lockup spin all` could not be joined");
        return 1;
    }
    if (!made) {
        return 1;
    }
    return 0;
}

/// |text| as a decimal number of at most MAX_DIGITS digits, or none.
auto parse_number(std::string_view const text) -> ustl::Option<u64> {
    if (text.empty() || text.size() > MAX_DIGITS) {
        return ustl::NONE;
    }
    auto value = u64{0};
    for (auto const c: text) {
        if (c < '0' || c > '9') {
            return ustl::NONE;
        }
        value = value * 10 + static_cast<u64>(c - '0');
    }
    return ustl::Option<u64>(value);
}

/// `lockup spin`'s arguments, argv[2] on; usage if they are not `all`, or
/// a CPU and an optional length.
auto spin_command(int const argc, char **const argv) -> int {
    if (argc == 3 && std::string_view(argv[2]) == "all") {
        return spin_all();
    }
    if (argc != 3 && argc != 4) {
        log::warn("usage: lockup spin <cpu> [<ms>] | lockup spin all");
        return 1;
    }
    auto const cpu = parse_number(argv[2]);
    if (cpu.is_none()) {
        log::warn("usage: lockup spin <cpu> [<ms>]: <cpu> is a CPU number, or `all`");
        return 1;
    }
    auto length = SpinLength();
    if (argc == 4) {
        auto const ms = parse_number(argv[3]);
        if (ms.is_none()) {
            log::warn("usage: lockup spin <cpu> [<ms>]: <ms> is a number of milliseconds");
            return 1;
        }
        length = SpinLength(Milliseconds(static_cast<Milliseconds::rep>(ms.value())));
    }
    return spin(static_cast<CpuNum>(cpu.value()), length);
}

auto lockup_command(int argc, char **argv) -> int {
    if (argc == 2 && std::string_view(argv[1]) == "status") {
        // First, so that the checker's last line stays the status's last:
        // scripts/kshell_run.py ends the command there.
        ours::log_console_waits();
        lockup_detector::log_status();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "nmi") {
        return dump_by_nmi();
    }
    if (argc >= 2 && std::string_view(argv[1]) == "spin") {
        return spin_command(argc, argv);
    }
    log::warn("usage: lockup status | lockup nmi | lockup spin <cpu> [<ms>] | lockup spin all");
    return 1;
}
} // namespace

GKTL_COMMAND(Lockup, "lockup", lockup_command,
             "lockup status | nmi | spin <cpu> [<ms>] | spin all: show the checker's notes, dump "
             "every CPU's state through an NMI, or stop one CPU's tick, or every CPU's, to be "
             "found by the detector or an NMI")
