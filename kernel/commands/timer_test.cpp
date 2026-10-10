/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `timer.test`: two things about timers.
//
// Timer::forward(), the arithmetic a periodic timer keeps its phase by. It
// moves a deadline on by whole intervals until it is ahead of `now`, and
// answers how many deadlines that passed. A deadline equal to `now` is due --
// the queue runs a timer whose deadline is at or before now -- so it counts
// as passed and moves. The tick timer and the sampler's timers call forward()
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
// A console command rather than a unit test because Timer's arming goes
// through the per-CPU TimerQueue, which a host test cannot stand up.
#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/timer.hpp>
#include <ours/time/timer_queue.hpp>

#include <algorithm>
#include <array>
#include <atomic>

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

        // The whole run since boot, on every CPU, not only these samples.
        verdict(tally, TimerQueue::num_refused_programs() == 0,
                "no tick device has refused a deadline since boot");
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
