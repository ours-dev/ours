/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// What an idle CPU does with its time, measured over a window
// (.agents/tasks/idle-power, step 1):
//
//   idle.stats [<seconds>]   for each online CPU: the timer interrupts it took
//                            per second, the times it halted per second, and
//                            the share of the window it spent halted
//   idle.test                the same over one second, and the counters
//                            behind it checked for sense on a quiet machine:
//                            with one-shot tick devices, which have no tick,
//                            that no CPU is woken for nothing (step 4); with
//                            MWAIT, that waking an idle CPU writes its word
//                            rather than interrupting it (step 5)
//
// A timer interrupt is a run of the CPU's expired timers
// (TimerQueue::num_runs()), whichever device or IPI delivered it; a halt is
// one wait in the idle loop, `hlt` or `mwait`, and the time halted is the
// wait alone (IdleThread::num_halts(), halted_time()). With `mwait`, a wait a
// waker ended by writing the CPU's monitor word, not by an IPI, is counted
// too (ArchIdleThread::num_monitor_wakes()).
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/timer_queue.hpp>

#include <string_view>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>

namespace ours {
namespace {
    using task::IdleThread;

    /// The longest window idle.stats measures, so a mistyped number does not
    /// hold the console for hours.
    constexpr u64 MAX_SECONDS = 600;
    constexpr u64 DEFAULT_SECONDS = 5;

    /// One CPU's counters at one moment.
    struct Sample {
        u64 timer_interrupts;
        u64 halts;
        u64 monitor_wakes;
        Duration halted;
    };

    /// Every online CPU's counters, and when they were read.
    struct Snapshot {
        CpuMask cpus;
        TimePoint<> at;
        Sample per_cpu[MAX_CPU];
    };

    auto take(Snapshot *const out) -> void {
        out->cpus = Cpu::online_mask();
        out->at = time::SteadyClock::now();
        out->cpus.for_each([out](CpuNum const cpu) {
            out->per_cpu[cpu] = Sample{
                .timer_interrupts = time::TimerQueue::num_runs(cpu),
                .halts = IdleThread::num_halts(cpu),
                .monitor_wakes = ArchIdleThread::num_monitor_wakes(cpu),
                .halted = IdleThread::halted_time(cpu, out->at),
            };
        });
    }

    /// What one CPU did between two snapshots.
    struct Window {
        Duration length;
        u64 timer_interrupts;
        u64 halts;
        u64 monitor_wakes;
        Duration halted;

        /// |count| per second over the window, in tenths: an idle CPU with no
        /// tick takes about one timer interrupt a second, which whole numbers
        /// round down to zero over a window a hair longer than its seconds.
        [[nodiscard]]
        auto per_second_tenths(u64 const count) const -> u64 {
            auto const ns = static_cast<u64>(Nanoseconds(length).count());
            if (ns == 0) {
                return 0;
            }
            return count * 10'000'000'000 / ns;
        }

        /// The share of the window spent halted, in tenths of a percent.
        [[nodiscard]]
        auto halted_permille() const -> u64 {
            auto const ns = static_cast<u64>(Nanoseconds(length).count());
            if (ns == 0) {
                return 0;
            }
            return static_cast<u64>(Nanoseconds(halted).count()) * 1000 / ns;
        }
    };

    auto between(Snapshot const &before, Snapshot const &after, CpuNum const cpu) -> Window {
        auto const &a = before.per_cpu[cpu];
        auto const &b = after.per_cpu[cpu];
        return Window{
            .length = after.at - before.at,
            .timer_interrupts = b.timer_interrupts - a.timer_interrupts,
            .halts = b.halts - a.halts,
            .monitor_wakes = b.monitor_wakes - a.monitor_wakes,
            .halted = b.halted - a.halted,
        };
    }

    /// Two snapshots |seconds| apart, the calling thread asleep in between
    /// so that it does not count against the CPUs it measures.
    auto measure(u64 const seconds, Snapshot *const before, Snapshot *const after) -> void {
        take(before);
        auto _ = task::Thread::Current::sleep_for(Seconds(static_cast<i64>(seconds)),
                                                  task::Interruptible::No);
        take(after);
    }

    auto parse_seconds(std::string_view const text) -> ustl::Option<u64> {
        if (text.empty() || text.size() > 3) {
            return ustl::NONE;
        }
        auto value = u64{0};
        for (auto const c: text) {
            if (c < '0' || c > '9') {
                return ustl::NONE;
            }
            value = value * 10 + static_cast<u64>(c - '0');
        }
        if (value == 0 || value > MAX_SECONDS) {
            return ustl::NONE;
        }
        return ustl::Option<u64>(value);
    }

    // Static: two snapshots of MAX_CPU samples each are more than a kernel
    // stack should carry. One command runs at a time on the console.
    Snapshot BEFORE;
    Snapshot AFTER;

    /// One line per CPU online across the window, under |name|.
    auto report(char const *const name, u64 const seconds) -> void {
        auto const cpus = BEFORE.cpus & AFTER.cpus;
        auto const mwait = ArchIdleThread::waits_in_mwait();
        auto how = "hlt";
        if (mwait) {
            how = "mwait";
        }
        log::info("{}: over {} s, idle CPUs wait in {}", name, seconds, how);
        cpus.for_each([name, mwait](CpuNum const cpu) {
            auto const window = between(BEFORE, AFTER, cpu);
            auto const permille = window.halted_permille();
            auto const interrupts = window.per_second_tenths(window.timer_interrupts);
            auto const halts = window.per_second_tenths(window.halts);
            if (!mwait) {
                log::info("{}: cpu {}: {}.{} timer interrupts/s, {}.{} halts/s, {}.{}% halted",
                          name, cpu, interrupts / 10, interrupts % 10, halts / 10, halts % 10,
                          permille / 10, permille % 10);
                return;
            }
            auto const written = window.per_second_tenths(window.monitor_wakes);
            log::info("{}: cpu {}: {}.{} timer interrupts/s, {}.{} halts/s, {}.{} of them ended "
                      "by a write, {}.{}% halted",
                      name, cpu, interrupts / 10, interrupts % 10, halts / 10, halts % 10,
                      written / 10, written % 10, permille / 10, permille % 10);
        });
    }

    auto idle_stats_command(int argc, char **argv) -> int {
        auto seconds = DEFAULT_SECONDS;
        if (argc == 2) {
            auto const parsed = parse_seconds(argv[1]);
            if (parsed.is_none()) {
                log::warn("usage: idle.stats [<seconds>], 1 to {}", MAX_SECONDS);
                return 1;
            }
            seconds = parsed.value();
        } else if (argc > 2) {
            log::warn("usage: idle.stats [<seconds>], 1 to {}", MAX_SECONDS);
            return 1;
        }

        measure(seconds, &BEFORE, &AFTER);
        report("idle.stats", seconds);
        return 0;
    }

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("idle.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("idle.test: FAIL  {}", name);
    }

    /// Slack for the clock reads: a halt's own reads of the clock and the
    /// snapshot's are a few instructions apart.
    constexpr auto SLACK = Milliseconds(1);

    /// The most timer interrupts a CPU of a quiet machine takes in the second
    /// idle.test measures, when nothing ticks: its lockup heartbeat, once or
    /// twice as the window falls, and the measuring thread's own sleep. A
    /// tick would be a hundred.
    constexpr u64 MAX_QUIET_INTERRUPTS = 5;

    /// How many times idle.test wakes an idle CPU to see one woken by a write.
    /// Under QEMU's TCG, MWAIT is a PAUSE and an idle CPU spins through its
    /// loop, its word saying Waiting for a sliver of each turn: a waker found
    /// it so 1 or 2 times in 20 (2026-10-14), so a hundred leave about one
    /// chance in two thousand of none. On hardware, every wake finds it.
    constexpr usize WAKES = 100;
    constexpr auto JOIN_WITHIN = Seconds(5);

    auto do_nothing(void *) -> i32 {
        return 0;
    }

    /// Wakes |cpu| WAKES times, each by a thread placed on it, and answers
    /// how many of its waits a write ended meanwhile.
    auto wake_by_threads(CpuNum const cpu) -> ustl::Option<u64> {
        auto const before = ArchIdleThread::num_monitor_wakes(cpu);
        for (usize i = 0; i < WAKES; ++i) {
            auto *const thread =
                task::Thread::spawn("idle.test/wake", do_nothing, nullptr, task::DEFAULT_PRIORITY);
            if (thread == nullptr) {
                return ustl::NONE;
            }
            thread->set_cpu_affinity_hard(CpuMask::of(cpu));
            auto _ = thread->resume();
            if (thread->join(nullptr, time::Deadline::at(time::SteadyClock::now() + JOIN_WITHIN)) !=
                Status::Ok) {
                return ustl::NONE;
            }
        }
        return ustl::Option<u64>(ArchIdleThread::num_monitor_wakes(cpu) - before);
    }

    auto idle_test_command(int, char **) -> int {
        Tally tally;
        measure(1, &BEFORE, &AFTER);
        report("idle.test", 1);
        auto const cpus = BEFORE.cpus & AFTER.cpus;

        auto none_over = true;
        auto mostly_halted = INVALID_CPU_NUM;
        cpus.for_each([&none_over, &mostly_halted](CpuNum const cpu) {
            auto const window = between(BEFORE, AFTER, cpu);
            none_over &= window.halted <= window.length + SLACK;
            if (mostly_halted == INVALID_CPU_NUM && window.halted_permille() >= 500 &&
                window.halts > 0) {
                mostly_halted = cpu;
            }
        });
        verdict(tally, none_over, "no CPU is halted for longer than the window");
        verdict(tally, mostly_halted != INVALID_CPU_NUM,
                "on a quiet machine some CPU halts, and for at least half the window");

        if (time::TickDevice::Current::is_mode_oneshot()) {
            auto none_woken = true;
            cpus.for_each([&none_woken](CpuNum const cpu) {
                none_woken &= between(BEFORE, AFTER, cpu).timer_interrupts <= MAX_QUIET_INTERRUPTS;
            });
            verdict(tally, none_woken,
                    "with no tick, no CPU takes more than a few timer interrupts");
        }

        if (ArchIdleThread::waits_in_mwait()) {
            auto target = INVALID_CPU_NUM;
            cpus.for_each([&target](CpuNum const cpu) {
                if (cpu != Cpu::id()) {
                    target = cpu;
                }
            });
            // With no CPU but this one there is nothing to wake, and so nothing
            // to fail.
            auto written = ustl::Option<u64>(1);
            if (target != INVALID_CPU_NUM) {
                written = wake_by_threads(target);
            }
            if (written.is_some()) {
                log::info("idle.test: cpu {}: {} of {} wakes ended by a write", target,
                          written.value(), WAKES);
            }
            // At most two writes a wake: the thread placed there, and the one
            // its exit may wake there. A count past that is a CPU that did
            // not wait at all, coming out to a word it never set.
            verdict(tally, written.is_some() && written.value() > 0 && written.value() <= 2 * WAKES,
                    "with mwait, waking an idle CPU writes its word rather than interrupting it");
        }

        if (tally.failed != 0) {
            log::error("idle.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("idle.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(
    IdleStats, "idle.stats", ours::idle_stats_command,
    "idle.stats [<seconds>]: timer interrupts, halts and time halted per CPU, over a window");
GKTL_COMMAND(IdleTest, "idle.test", ours::idle_test_command,
             "check the idle counters over one second of a quiet machine");
