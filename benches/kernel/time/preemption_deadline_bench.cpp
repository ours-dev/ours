/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `preemptdeadline.bench`: what picking a thread costs its CPU's timer queue
// and timer device (docs/repairs/261013/kernel-time.md item 1). Each pick
// sets the CPU's preemption deadline (TimerQueue::set_preemption_deadline()), which
// reads the clock, takes the queue's lock, and writes the device when the
// earliest deadline changed, reading the clock again to do it. Measured:
//
//   per switch     two threads on one CPU hand a token back and forth: how
//                  many times each switch sets the deadline, and how many
//                  times it writes the device. Counts, the same on any
//                  machine.
//   now()          one read of the steady clock.
//   lock           a spin lock taken and released, interrupts off.
//   set same       set_preemption_deadline() with the deadline the device holds
//                  already: the lock and the compare, no device write.
//   set new        set_preemption_deadline() with a new, earliest deadline each time:
//                  the device written.
//   switch         one switch of the hand-off above.
//
// Every timing rests on the processor and on how fast the clock and the
// timer device answer, which an emulator models not at all, so each needs
// hardware (bench::Needs::Hardware).
//
// `csv` as the last argument also prints every raw sample, one line each, as
// `preemptdeadline.bench: csv: case,metric,unit,value`: the rows a performance
// report keeps (docs/perf/TEMPLATE.md). The hand-off is run HAND_OFF_RUNS
// times, a sample each; each timing of a piece is BATCHES samples.
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
#include <string_view>

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
    constexpr usize HAND_OFF_RUNS = 10;
    constexpr usize ROUND_TRIPS = 200;
    constexpr usize SWITCHES = 2 * ROUND_TRIPS;
    constexpr auto JOIN_WITHIN = Seconds(30);
    /// How far ahead the deadlines `reset new` sets are: closer than any
    /// timer the CPU is likely to hold, so that each is the earliest and is
    /// written to the device.
    constexpr auto NEAR = Microseconds(100);

    /// Where the run's lines go, and whether its raw samples do too.
    struct Output {
        bench::Run const &run;
        bool csv;

        /// One raw sample; |value| in thousandths, printed with three places.
        auto sample(char const *const what, char const *const metric, char const *const unit,
                    u64 const milli) const -> void {
            if (csv) {
                log::info("{}: csv: {},{},{},{}.{:03}", run.name(), what, metric, unit,
                          milli / 1000, milli % 1000);
            }
        }
    };

    /// Sorts |values| in place: an insertion sort, for a hundred values, and
    /// because std::sort's out-of-line half is not in the kernel.
    auto sort(u64 *const values, usize const count) -> void {
        for (usize i = 1; i < count; ++i) {
            for (usize j = i; j > 0 && values[j - 1] > values[j]; --j) {
                auto const swapped = values[j];
                values[j] = values[j - 1];
                values[j - 1] = swapped;
            }
        }
    }

    /// The per-call time of |op| in BATCHES batches of PER_BATCH calls, each
    /// batch a sample; the median and 99th percentile summed up. A batch is
    /// timed as a whole, so the two clock reads that bound it cost each call
    /// a hundredth of a read.
    template <typename Op>
    auto time_calls(Output const &out, char const *const what, Op &&op) -> void {
        u64 per_call[BATCHES];
        for (auto &ns: per_call) {
            auto const start = SteadyClock::now();
            for (usize i = 0; i < PER_BATCH; ++i) {
                op(i);
            }
            ns = static_cast<u64>(Nanoseconds(SteadyClock::now() - start).count()) / PER_BATCH;
            out.sample(what, "time", "ns/call", ns * 1000);
        }
        sort(per_call, BATCHES);
        log::info("{}: {}: median {} ns, p99 {} ns per call", out.run.name(), what,
                  per_call[BATCHES / 2], per_call[BATCHES * 99 / 100]);
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
        auto const resets = TimerQueue::num_preemption_deadline_sets(hand_off.cpu);
        auto const programs = TimerQueue::num_programs(hand_off.cpu);
        auto const start = SteadyClock::now();
        for (usize i = 0; i < ROUND_TRIPS; ++i) {
            hand_off.ping.post();
            auto _ = hand_off.pong.wait();
        }
        hand_off.took = SteadyClock::now() - start;
        hand_off.resets = TimerQueue::num_preemption_deadline_sets(hand_off.cpu) - resets;
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

    /// HAND_OFF_RUNS hand-offs on the highest online CPU, a sample each.
    auto hand_offs(Output const &out) -> bool {
        Cpu::online_mask().for_each([](CpuNum const cpu) { HAND_OFF.cpu = cpu; });
        auto const timed = out.run.measures("switch", bench::Needs::Hardware);
        u64 resets = 0;
        u64 programs = 0;
        for (usize r = 0; r < HAND_OFF_RUNS; ++r) {
            auto *const second = spawn_on("preemptdeadline/2", second_routine, HAND_OFF);
            auto *const first = spawn_on("preemptdeadline/1", first_routine, HAND_OFF);
            if (!join(first) || !join(second)) {
                log::error("{}: the hand-off did not finish", out.run.name());
                return false;
            }
            resets += HAND_OFF.resets;
            programs += HAND_OFF.programs;
            out.sample("hand-off", "deadline sets", "per switch",
                       HAND_OFF.resets * 1000 / SWITCHES);
            out.sample("hand-off", "device writes", "per switch",
                       HAND_OFF.programs * 1000 / SWITCHES);
            if (timed) {
                out.sample("hand-off", "time", "ns/switch",
                           static_cast<u64>(Nanoseconds(HAND_OFF.took).count()) * 1000 / SWITCHES);
            }
        }
        auto const switches = HAND_OFF_RUNS * SWITCHES;
        log::info("{}: per switch, on CPU {}: {}.{:02} deadline sets, {}.{:02} device writes "
                  "({} switches)",
                  out.run.name(), HAND_OFF.cpu, resets / switches, resets * 100 / switches % 100,
                  programs / switches, programs * 100 / switches % 100, switches);
        return true;
    }

    auto preemption_deadline_bench(int argc, char **argv) -> int {
        // `csv` is this benchmark's own, and the rest is bench::Run's.
        auto const csv = argc > 1 && std::string_view(argv[argc - 1]) == "csv";
        auto run_argc = argc;
        if (csv) {
            run_argc -= 1;
        }
        auto const run = bench::Run("preemptdeadline.bench", run_argc, argv);
        if (!run.arguments_ok()) {
            log::warn("usage: preemptdeadline.bench [all] [csv]");
            return 1;
        }
        auto const out = Output{.run = run, .csv = csv};

        if (!hand_offs(out)) {
            return 1;
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
                // Written and never read: it keeps the reads from being
                // optimised away.
                [[maybe_unused]]
                auto sink = TimePoint<>();
                time_calls(out, "now()", [&sink](usize) { sink = SteadyClock::now(); });
            }
            if (run.measures("lock", bench::Needs::Hardware)) {
                auto lock = sync::SpinLock();
                time_calls(out, "lock",
                           [&lock](usize) { Guard<sync::SpinLock, IrqSave> const guard(lock); });
            }
            if (run.measures("set same", bench::Needs::Hardware)) {
                auto const deadline = SteadyClock::now() + NEAR;
                queue.set_preemption_deadline(deadline);
                time_calls(out, "set same",
                           [&queue, deadline](usize) { queue.set_preemption_deadline(deadline); });
            }
            if (run.measures("set new", bench::Needs::Hardware)) {
                // A nanosecond later each time: a deadline the device does
                // not hold yet, and still the earliest.
                auto const base = SteadyClock::now() + NEAR;
                auto serial = i64{0};
                time_calls(out, "set new", [&queue, base, &serial](usize) {
                    serial += 1;
                    queue.set_preemption_deadline(base + Nanoseconds(serial));
                });
            }
        }
        return run.finish();
    }
} // namespace
} // namespace ours

GKTL_COMMAND(PreemptionDeadlineBench, "preemptdeadline.bench", ours::preemption_deadline_bench,
             "preemptdeadline.bench [all] [csv]: what picking a thread costs its timer queue and "
             "device");
