/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `timer.test`: Timer::forward(), the arithmetic a periodic timer keeps its
// phase by.
//
// forward() moves a deadline on by whole intervals until it is ahead of
// `now`, and answers how many deadlines that passed. A deadline equal to
// `now` is due -- the queue runs a timer whose deadline is at or before now --
// so it counts as passed and moves. The tick timer and the sampler's timers
// call forward() from their own handler, which can run at exactly the
// deadline; one that stayed put would be popped again by the same expiry
// pass.
//
// The timer is armed far in the future and cancelled at once: cancel() leaves
// the deadline in place, which is the only way to give a Timer a deadline
// without leaving it in a queue, and forward() refuses a queued timer.
//
// A console command rather than a unit test because Timer's arming goes
// through the per-CPU TimerQueue, which a host test cannot stand up.
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>

#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using time::Deadline;
    using time::SteadyClock;
    using time::Timer;

    constexpr auto INTERVAL = Milliseconds(10);
    /// Far enough ahead that the timer cannot fire before the cancel().
    constexpr auto FAR_AHEAD = Seconds(3600);

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
             "forward a timer's deadline and check the count and where it lands");
