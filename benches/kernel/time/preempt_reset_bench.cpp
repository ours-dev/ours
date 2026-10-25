/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `preemptreset.bench`: what picking a thread costs its CPU's timer queue
// and timer device (docs/repairs/261013/kernel-time.md item 1). Each pick
// sets the CPU's preemption deadline (TimerQueue::reset_preempt()), which
// reads the clock, takes the queue's lock, and writes the device when the
// earliest deadline changed, reading the clock again to do it. Measured:
//
//   per switch     two threads on one CPU hand a token back and forth: how
//                  many times each switch sets the deadline, and how many
//                  times it writes the device. Counts, the same on any
//                  machine.
//   now()          one read of the steady clock.
//   lock           a spin lock taken and released, interrupts off.
//   reset, same    reset_preempt() with the deadline the device holds
//                  already: the lock and the compare, no device write.
//   reset, new     reset_preempt() with a new, earliest deadline each time:
//                  the device written.
//   switch         one switch of the hand-off above.
//
// Every timing rests on the processor and on how fast the clock and the
// timer device answer, which an emulator models not at all, so each needs
// hardware (bench::Needs::Hardware).
//
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/lockdep.hpp>
#include <ours/sync/semaphore.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer_queue.hpp>

#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <bench/run.hpp>
#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using task::Thread;
    using time::SteadyClock;
    using time::TimerQueue;

    constexpr usize BATCHES = 100;
    constexpr usize PER_BATCH = 100;
    constexpr usize ROUND_TRIPS = 2000;
    constexpr auto JOIN_WITHIN = Seconds(30);
    /// How far ahead the deadlines `reset, new` sets are: closer than any
    /// timer the CPU is likely to hold, so that each is the earliest and is
    /// written to the device.
    constexpr auto NEAR = Microseconds(100);

    /// The middle and the 99th percentile of the per-call time over BATCHES
    /// batches of PER_BATCH calls. A batch is timed as a whole, so the two
    /// clock reads that bound it cost each call a hundredth of a read.
    struct Spread {
        u64 median_ns;
        u64 p99_ns;
    };

    template <typename Op>
    auto time_calls(Op &&op) -> Spread {
        u64 per_call[BATCHES];
        for (auto &ns: per_call) {
            auto const start = SteadyClock::now();
            for (usize i = 0; i < PER_BATCH; ++i) {
                op(i);
            }
            ns = static_cast<u64>(Nanoseconds(SteadyClock::now() - start).count()) / PER_BATCH;
        }
        // Insertion sort: a hundred values, and std::sort's out-of-line half
        // is not in the kernel.
        for (usize i = 1; i < BATCHES; ++i) {
            for (usize j = i; j > 0 && per_call[j - 1] > per_call[j]; --j) {
                auto const swapped = per_call[j];
                per_call[j] = per_call[j - 1];
                per_call[j - 1] = swapped;
            }
        }
        return Spread{.median_ns = per_call[BATCHES / 2], .p99_ns = per_call[BATCHES * 99 / 100]};
    }

    auto report(bench::Run const &run, char const *const metric, Spread const spread) -> void {
        log::info("{}: {}: median {} ns, p99 {} ns per call", run.name(), metric, spread.median_ns,
                  spread.p99_ns);
    }

    /// The hand-off: |ping| takes the token to the second thread, |pong| back.
    /// The first thread reads the counters and the clock at either end.
    struct HandOff {
        CpuNum cpu = INVALID_CPU_NUM;
        sync::Semaphore ping;
        sync::Semaphore pong;
        u64 resets = 0;
        u64 programs = 0;
        Duration took{};
    };

    auto first_routine(void *const arg) -> i32 {
        auto &hand_off = *static_cast<HandOff *>(arg);
        auto const resets = TimerQueue::num_preempt_resets(hand_off.cpu);
        auto const programs = TimerQueue::num_programs(hand_off.cpu);
        auto const start = SteadyClock::now();
        for (usize i = 0; i < ROUND_TRIPS; ++i) {
            hand_off.ping.post();
            auto _ = hand_off.pong.wait();
        }
        hand_off.took = SteadyClock::now() - start;
        hand_off.resets = TimerQueue::num_preempt_resets(hand_off.cpu) - resets;
        hand_off.programs = TimerQueue::num_programs(hand_off.cpu) - programs;
        return 0;
    }

    auto second_routine(void *const arg) -> i32 {
        auto &hand_off = *static_cast<HandOff *>(arg);
        for (usize i = 0; i < ROUND_TRIPS; ++i) {
            auto _ = hand_off.ping.wait();
            hand_off.pong.post();
        }
        return 0;
    }

    auto spawn_on(char const *const name, Thread::StartRoutine const routine, HandOff &hand_off)
        -> Thread * {
        auto *const thread = Thread::spawn(name, routine, &hand_off, task::DEFAULT_PRIORITY);
        if (thread != nullptr) {
            thread->set_cpu_affinity_hard(CpuMask::of(hand_off.cpu));
            auto _ = thread->resume();
        }
        return thread;
    }

    auto join(Thread *const thread) -> bool {
        return thread != nullptr &&
               thread->join(nullptr, time::Deadline::at(SteadyClock::now() + JOIN_WITHIN)) ==
                   Status::Ok;
    }

    HandOff HAND_OFF;

    auto preempt_reset_bench(int argc, char **argv) -> int {
        auto const run = bench::Run("preemptreset.bench", argc, argv);
        if (!run.arguments_ok()) {
            log::warn("usage: preemptreset.bench [all]");
            return 1;
        }

        // The hand-off, on the highest online CPU.
        Cpu::online_mask().for_each([](CpuNum const cpu) { HAND_OFF.cpu = cpu; });
        auto *const second = spawn_on("preemptreset/2", second_routine, HAND_OFF);
        auto *const first = spawn_on("preemptreset/1", first_routine, HAND_OFF);
        if (!join(first) || !join(second)) {
            log::error("{}: the hand-off did not finish", run.name());
            return 1;
        }
        auto const switches = 2 * ROUND_TRIPS;
        log::info("{}: per switch, on CPU {}: {}.{:02} deadline sets, {}.{:02} device writes "
                  "({} switches)",
                  run.name(), HAND_OFF.cpu, HAND_OFF.resets / switches,
                  HAND_OFF.resets * 100 / switches % 100, HAND_OFF.programs / switches,
                  HAND_OFF.programs * 100 / switches % 100, switches);
        if (run.measures("switch", bench::Needs::Hardware)) {
            log::info("{}: switch: {} ns each, averaged over {}", run.name(),
                      static_cast<u64>(Nanoseconds(HAND_OFF.took).count()) / switches, switches);
        }

        // The pieces of one pick, on this CPU, with interrupts and preemption
        // off so that nothing lands inside a batch. The last deadline set
        // here passes once they are on again; the preemption it asks for
        // gives this thread a fresh one.
        {
            task::AutoPreemptDisabler const no_preemption;
            arch::AutoInterruptDisabler const no_interrupts;
            auto &queue = TimerQueue::get();

            if (run.measures("now()", bench::Needs::Hardware)) {
                auto sink = TimePoint<>();
                report(run, "now()", time_calls([&sink](usize) { sink = SteadyClock::now(); }));
                static_cast<void>(sink);
            }
            if (run.measures("lock", bench::Needs::Hardware)) {
                auto lock = sync::SpinLock();
                report(run, "lock", time_calls([&lock](usize) {
                           Guard<sync::SpinLock, IrqSave> const guard(lock);
                       }));
            }
            if (run.measures("reset, same", bench::Needs::Hardware)) {
                auto const deadline = SteadyClock::now() + NEAR;
                queue.reset_preempt(deadline);
                report(run, "reset, same",
                       time_calls([&queue, deadline](usize) { queue.reset_preempt(deadline); }));
            }
            if (run.measures("reset, new", bench::Needs::Hardware)) {
                // A nanosecond later each time: a deadline the device does
                // not hold yet, and still the earliest.
                auto const base = SteadyClock::now() + NEAR;
                auto serial = i64{0};
                report(run, "reset, new", time_calls([&queue, base, &serial](usize) {
                           serial += 1;
                           queue.reset_preempt(base + Nanoseconds(serial));
                       }));
            }
        }
        return run.finish();
    }
} // namespace
} // namespace ours

GKTL_COMMAND(PreemptResetBench, "preemptreset.bench", ours::preempt_reset_bench,
             "preemptreset.bench [all]: what picking a thread costs its timer queue and device");
