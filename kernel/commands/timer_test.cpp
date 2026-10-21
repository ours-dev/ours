/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `timer.test`: two things about timers.
//
// Timer::forward(), the arithmetic a periodic timer keeps its phase by. It
// moves a deadline on by whole intervals until it is ahead of `now`, and
// answers how many deadlines that passed. A deadline equal to `now` is due --
// the queue runs a timer whose deadline is at or before now -- so it counts
// as passed and moves. The lockup heartbeat and the sampler's timers call forward()
// from their own handler, which can run at exactly the deadline; one that
// stayed put would be popped again by the same expiry pass. The timer is
// armed far in the future and cancelled at once: cancel() leaves the
// deadline in place, which is the only way to give a Timer a deadline without
// leaving it in a queue, and forward() refuses a queued timer.
//
// How late a short timer fires. SAMPLES timers of one millisecond each are
// armed one after another, at phases spread across the tick period, and the
// time each one fired after its deadline is logged. With the tick device
// periodic a timer waits for the next tick, so its lateness is spread over
// the whole period, about half of it at the median. With kernel.time.oneshot
// on the device is armed for the timer itself, and the median must be well
// under a tick; that is asserted, and only then. Under QEMU's TCG the
// figures include the host's scheduling, so the bound is loose.
//
// What a slow pass does to the next one. A pass that ran a callback holds the
// device off for as long as it took (TimerQueue::m_hold_off_until), so that
// the thread it interrupted is left as much time as the callbacks took. Timer
// A's callback takes two milliseconds; timer B is due one and a half
// milliseconds after A, so it falls due during A's pass and is not part of it.
// The interrupt that runs B must then come no sooner than about the length of
// that pass after the pass ended; without the hold-off it comes when the
// device can deliver it, a fraction of a millisecond. This is also what the
// hold-off costs every other timer on the CPU, said as a number. A trial in
// which the interrupt that began the pass came so late that B was due by then
// ran B in the same pass and says nothing, and is not counted (the CPU's count
// of passes tells: one pass for both); at least one trial must count.
//
// It needs kernel.time.oneshot; with the device periodic it is skipped.
//
// A console command rather than a unit test because Timer's arming goes
// through the per-CPU TimerQueue, which a host test cannot stand up.
#include <ours/cpu.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/timer.hpp>
#include <ours/time/timer_queue.hpp>

#include <algorithm>
#include <array>
#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using time::Deadline;
    using time::SteadyClock;
    using time::TickDevice;
    using time::Timer;
    using time::TimerQueue;

    constexpr auto INTERVAL = Milliseconds(10);
    /// Far enough ahead that the timer cannot fire before the cancel().
    constexpr auto FAR_AHEAD = Seconds(3600);

    constexpr usize SAMPLES = 24;
    /// What each sample asks for: a sixth of a tick.
    constexpr auto REQUESTED = Milliseconds(1);
    /// Added between samples, so that they land at different phases of the
    /// tick: 410 us against a 10 ms period drifts 4.1 % of it each time.
    constexpr auto PHASE_STEP = Microseconds(410);
    /// A sample that has not fired by now never will.
    constexpr auto LOST_AFTER = Milliseconds(200);
    /// Where the median of the lateness must lie when the device is one-shot:
    /// a fifth of a tick. Periodic puts it near half a tick.
    constexpr auto ONESHOT_MEDIAN_LIMIT = Milliseconds(2);

    /// How long A's callback runs, and when B is due after A.
    constexpr auto SLOW_PASS = Milliseconds(2);
    constexpr auto B_AFTER_A = Microseconds(1500);
    /// B must come at least this long after A's pass ended: nine tenths of the
    /// pass, for the clock reads at either end of it.
    constexpr auto HELD_AT_LEAST = Microseconds(1800);
    constexpr usize HOLD_OFF_TRIALS = 6;
    constexpr auto TRIAL_GIVE_UP = Milliseconds(200);

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto idle(Timer *) -> Timer::HandlerResult {
        return Timer::HandlerResult::Handled;
    }

    /// Forwards a timer whose deadline is |deadline| to |now|, and checks the
    /// count and where the deadline ended up.
    auto check_forward(Tally &tally, char const *const name, TimePoint<> const deadline,
                       TimePoint<> const now, u64 const expect_overrun,
                       TimePoint<> const expect_deadline) -> void {
        Timer timer;
        timer.activate(Deadline::at(deadline), idle);
        auto _ = timer.cancel();

        auto const overrun = timer.forward(now, INTERVAL);
        auto const moved_to = timer.deadline().when();
        if (overrun == expect_overrun && moved_to == expect_deadline) {
            tally.passed += 1;
            log::info("timer.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("timer.test: FAIL  {}: overrun {} (want {}), deadline moved by {} ns (want {})",
                   name, overrun, expect_overrun, (moved_to - deadline).count(),
                   (expect_deadline - deadline).count());
    }

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("timer.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("timer.test: FAIL  {}", name);
    }

    /// Written by the timer's handler, in the timer interrupt, and read by the
    /// command once |done| says so.
    struct Fired {
        TimePoint<> at;
        std::atomic<bool> done{false};
    };

    /// Arms one timer for REQUESTED from now and waits for it. False if it
    /// had not fired LOST_AFTER past its deadline; |lateness| is then unset.
    auto sample_lateness(Duration &lateness) -> bool {
        Fired fired;
        Timer timer;
        auto *const pf = &fired;
        auto const deadline = SteadyClock::now() + REQUESTED;
        timer.activate(Deadline::at(deadline), [pf](Timer *) {
            pf->at = SteadyClock::now();
            pf->done.store(true, std::memory_order_release);
            return Timer::HandlerResult::Handled;
        });

        auto const give_up = deadline + LOST_AFTER;
        auto const arrived = [&] {
            while (!fired.done.load(std::memory_order_acquire)) {
                if (SteadyClock::now() >= give_up) {
                    return false;
                }
                arch::spin_hint();
            }
            return true;
        }();

        // Either way: waits out a handler still running, after which |fired|
        // and |timer| are the command's again.
        auto _ = timer.cancel();
        if (arrived) {
            lateness = fired.at - deadline;
        }
        return arrived;
    }

    auto check_lateness(Tally &tally) -> void {
        // A difference and not the count since boot: another test (tickreplace.test)
        // refuses deadlines on purpose, and would fail this one if it ran first.
        auto const refused_before = TimerQueue::num_refused_programs();

        std::array<Duration, SAMPLES> lateness{};
        usize num_fired = 0;
        for (usize i = 0; i < SAMPLES; ++i) {
            // Moves the next sample to another phase of the tick period.
            auto const until = SteadyClock::now() + PHASE_STEP * static_cast<Duration::rep>(i);
            while (SteadyClock::now() < until) {
                arch::spin_hint();
            }
            if (sample_lateness(lateness[num_fired])) {
                num_fired += 1;
            }
        }
        verdict(tally, num_fired == SAMPLES, "every timer fired");
        if (num_fired == 0) {
            return;
        }

        std::sort(lateness.begin(), lateness.begin() + static_cast<isize>(num_fired));
        auto const median = lateness[num_fired / 2];
        auto const oneshot = TickDevice::Current::is_mode_oneshot();
        char const *device_mode = "periodic";
        if (oneshot) {
            device_mode = "one-shot";
        }
        log::info("timer.test:   {} us timers fired {} to {} us late, median {} us ({} samples, "
                  "tick device {})",
                  duration_cast<Microseconds>(REQUESTED).count(),
                  duration_cast<Microseconds>(lateness[0]).count(),
                  duration_cast<Microseconds>(lateness[num_fired - 1]).count(),
                  duration_cast<Microseconds>(median).count(), num_fired, device_mode);
        verdict(tally, lateness[0] >= Duration::zero(), "none fired before its deadline");

        if (oneshot) {
            verdict(tally, median < ONESHOT_MEDIAN_LIMIT,
                    "one-shot: the median lateness is under a fifth of a tick");
        }

        // On every CPU while this ran, not only for these samples.
        verdict(tally, TimerQueue::num_refused_programs() == refused_before,
                "no tick device refused a deadline meanwhile");
    }

    /// Written by the two callbacks, in the timer interrupt, and read by the
    /// command once the flags say so.
    struct SlowPass {
        TimePoint<> a_ended;
        TimePoint<> b_began;
        /// How many times this CPU ran its timers from the arming to the end.
        u64 passes = 0;
        std::atomic<bool> a_done{false};
        std::atomic<bool> b_done{false};
    };

    /// One trial: arms A and B, waits for both. False if one did not come.
    auto run_slow_pass(SlowPass &trial) -> bool {
        auto *const pt = &trial;
        Timer a;
        Timer b;
        CpuNum cpu = 0;
        u64 passes_before = 0;
        {
            // Both on this CPU's queue: the thread must not move between them.
            arch::AutoInterruptDisabler const no_interrupts;
            cpu = Cpu::id();
            passes_before = TimerQueue::num_runs(cpu);
            auto const a_due = SteadyClock::now() + Milliseconds(1);
            a.activate(Deadline::at(a_due), [pt](Timer *) {
                auto const until = SteadyClock::now() + SLOW_PASS;
                while (SteadyClock::now() < until) {
                    arch::spin_hint();
                }
                pt->a_ended = SteadyClock::now();
                pt->a_done.store(true, std::memory_order_release);
                return Timer::HandlerResult::Handled;
            });
            b.activate(Deadline::at(a_due + B_AFTER_A), [pt](Timer *) {
                pt->b_began = SteadyClock::now();
                pt->b_done.store(true, std::memory_order_release);
                return Timer::HandlerResult::Handled;
            });
        }

        auto const give_up = SteadyClock::now() + TRIAL_GIVE_UP;
        auto const arrived = [&] {
            while (!trial.a_done.load(std::memory_order_acquire) ||
                   !trial.b_done.load(std::memory_order_acquire)) {
                if (SteadyClock::now() >= give_up) {
                    return false;
                }
                arch::spin_hint();
            }
            return true;
        }();

        // Either way: waits out a handler still running.
        auto _ = a.cancel();
        auto _ = b.cancel();
        trial.passes = TimerQueue::num_runs(cpu) - passes_before;
        return arrived;
    }

    auto check_hold_off(Tally &tally) -> void {
        // In Periodic mode the device is not armed for a timer's deadline: A
        // and B wait for the same tick and run in one pass.
        if (!TickDevice::Current::is_mode_oneshot()) {
            log::info("timer.test:   the hold-off check needs kernel.time.oneshot; skipped");
            return;
        }

        usize ran = 0;
        usize counted = 0;
        usize held = 0;
        Duration shortest = Duration::max();
        for (usize i = 0; i < HOLD_OFF_TRIALS; ++i) {
            SlowPass trial;
            if (!run_slow_pass(trial)) {
                break;
            }
            ran += 1;

            // One pass for both: B was due when A's pass began.
            if (trial.passes < 2) {
                continue;
            }
            auto const gap = trial.b_began - trial.a_ended;
            counted += 1;
            shortest = std::min(shortest, gap);
            if (gap >= HELD_AT_LEAST) {
                held += 1;
            }
        }
        verdict(tally, ran == HOLD_OFF_TRIALS, "both timers of every trial fired");
        verdict(tally, counted != 0,
                "B fell due during A's pass and ran in an interrupt of its own, in some trial");
        if (counted != 0) {
            log::info("timer.test:   after a {} us pass, the next interrupt came {} us later at "
                      "the soonest ({} of {} trials counted)",
                      duration_cast<Microseconds>(SLOW_PASS).count(),
                      duration_cast<Microseconds>(shortest).count(), counted, ran);
        }
        verdict(tally, counted != 0 && held == counted,
                "and it came no sooner than about the pass's own length after the pass");
    }

    auto timer_test_command(int, char **) -> int {
        Tally tally;
        auto const deadline = SteadyClock::now() + FAR_AHEAD;

        check_forward(tally, "before the deadline: nothing moves", deadline,
                      deadline - Milliseconds(1), 0, deadline);
        check_forward(tally, "exactly at the deadline: one interval", deadline, deadline, 1,
                      deadline + INTERVAL);
        check_forward(tally, "half an interval late: one interval", deadline,
                      deadline + INTERVAL / 2, 1, deadline + INTERVAL);
        check_forward(tally, "exactly on the next deadline: two intervals", deadline,
                      deadline + INTERVAL, 2, deadline + 2 * INTERVAL);
        check_forward(tally, "two and a half intervals late: three intervals", deadline,
                      deadline + 5 * INTERVAL / 2, 3, deadline + 3 * INTERVAL);

        check_lateness(tally);
        check_hold_off(tally);

        if (tally.failed != 0) {
            log::error("timer.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("timer.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(TimerTest, "timer.test", ours::timer_test_command,
             "forward a timer's deadline, and measure how late short timers fire");
