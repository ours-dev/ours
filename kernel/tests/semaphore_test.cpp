/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Semaphore.*: sync::Semaphore, the counting semaphore a port's waiters sleep
// on (.agents/tasks/microkernel-interrupts/PLAN.md, step 3). Its one invariant
// is that a thread waits only while the count is zero: a post hands its unit to
// a waiter if there is one and banks it otherwise, and a wait takes a banked
// unit if there is one and blocks otherwise. Each test below looks at one side
// of that:
//
//   - BanksPostsWithNoWaiter: posts with nobody waiting are banked, and the
//     next poll answers TimeOut;
//   - PostHandsUnitToAParkedWaiter: a wait with nothing banked blocks until
//     another thread posts, and the post hands its unit over;
//   - TimedWaitTimesOutAndLeavesTheQueue: a wait with a deadline times out and
//     leaves the queue, so a later post is banked;
//   - PostFromTimerHandlerWakesAParkedWaiter: a post from a timer handler, in
//     interrupt context, wakes a parked waiter;
//   - EachPostWakesExactlyOne: with several waiters, each post wakes one.
//
// In-kernel rather than a host test for the reason mutex.stress gives: a
// semaphore's slow path is the scheduler.
#include <ours/mm/gaf.hpp>
#include <ours/sync/semaphore.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>

#include <array>
#include <atomic>

#include <heap/new.hpp>
#include <ktest/ktest.hpp>

namespace ours {
namespace {

    using task::Thread;

    /// How long a wake may take before the test calls it lost. Generous for
    /// the reason waitqueue.test's is: under QEMU's TCG every park and wake
    /// is a pair of context switches on emulated CPUs.
    constexpr auto LOST_AFTER = Seconds(5);
    constexpr auto POLL_INTERVAL = Milliseconds(1);
    constexpr auto TIMEOUT = Milliseconds(5);
    /// How long the crowd check waits to see that one post woke no more
    /// than one thread.
    constexpr auto SETTLE = Milliseconds(20);

    /// Units banked by the first test.
    constexpr usize BANKED = 8;
    /// Threads waiting at once in the crowd test.
    constexpr usize CROWD = 4;

    /// A deadline long past, which makes a wait a poll.
    auto already_passed() -> time::Deadline {
        return time::Deadline::at(TimePoint<>{});
    }

    auto lost_after_from_now() -> time::Deadline {
        return time::Deadline::at(time::SteadyClock::now() + LOST_AFTER);
    }

    /// Polls |condition| until it holds or LOST_AFTER has passed.
    template <typename Condition>
    auto poll_until(Condition const &condition) -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (!condition()) {
            if (time::SteadyClock::now() >= give_up) {
                return false;
            }
            auto _ = Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }
        return true;
    }

    /// Spawns |routine| on |arg| and starts it; null if it cannot.
    auto start(char const *const name, Thread::StartRoutine const routine, void *const arg)
        -> Thread * {
        auto *const thread = Thread::spawn(name, routine, arg, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        auto _ = thread->resume();
        return thread;
    }

    auto join(Thread *const thread) -> bool {
        i32 retcode = 0;
        return thread->join(&retcode, time::Deadline::infinite()) == Status::Ok;
    }

    KTEST(Semaphore, BanksPostsWithNoWaiter) {
        sync::Semaphore sema;
        for (usize i = 0; i < BANKED; ++i) {
            sema.post();
        }
        EXPECT_EQ(sema.count(), BANKED, "posts with nobody waiting are banked");

        usize taken = 0;
        while (taken < BANKED && sema.wait(already_passed()) == Status::Ok) {
            taken += 1;
        }
        EXPECT_EQ(taken, BANKED, "as many polls as posts take a unit each");
        EXPECT_EQ(sema.wait(already_passed()), Status::TimeOut, "the next poll answers TimeOut");
    }

    /// Shared between the test, which waits, and the thread that posts once it
    /// has seen the test parked. On the heap, and freed only once the poster
    /// is joined: if the join fails, it is leaked rather than freed under a
    /// thread that may still use it.
    struct Handoff {
        sync::Semaphore sema;
        std::atomic<bool> saw_waiter{false};
    };

    auto handoff_poster(void *const arg) -> i32 {
        auto &h = *static_cast<Handoff *>(arg);
        auto const parked = poll_until([&h] { return h.sema.num_waiters() == 1; });
        h.saw_waiter.store(parked, std::memory_order_release);
        // Posts even if the waiter was never seen, so it is not left parked.
        h.sema.post();
        return 0;
    }

    KTEST(Semaphore, PostHandsUnitToAParkedWaiter) {
        auto *const h = new (mm::GAF_KERNEL) Handoff{};
        ASSERT_NONNULL(h, "allocate the handoff case");

        auto *const poster = start("semaphore-test-poster", handoff_poster, h);
        if (poster == nullptr) {
            delete h;
            FAIL("spawn the poster");
        }

        auto const status = h->sema.wait(lost_after_from_now());
        if (!join(poster)) {
            FAIL("join the poster; leaking its state");
        }

        EXPECT_EQ(status, Status::Ok, "a wait with nothing banked returns once posted");
        EXPECT_TRUE(h->saw_waiter.load(std::memory_order_acquire),
                    "and it was parked until the post");
        EXPECT_EQ(h->sema.count(), u64{0}, "the post handed its unit over rather than banking it");
        delete h;
    }

    KTEST(Semaphore, TimedWaitTimesOutAndLeavesTheQueue) {
        sync::Semaphore sema;
        auto const started = time::SteadyClock::now();
        auto const status = sema.wait(time::Deadline::at(started + TIMEOUT));
        auto const elapsed = time::SteadyClock::now() - started;

        auto const elapsed_us = duration_cast<Microseconds>(elapsed).count();
        EXPECT_EQ(status, Status::TimeOut, "a wait that nothing posts times out after {} us",
                  elapsed_us);
        EXPECT_TRUE(elapsed >= TIMEOUT, "and not before its deadline ({} us)", elapsed_us);
        EXPECT_EQ(sema.num_waiters(), usize{0}, "the timed-out waiter has left the queue");

        sema.post();
        EXPECT_EQ(sema.count(), u64{1}, "so a post after it is banked, not lost on it");
        EXPECT_EQ(sema.wait(already_passed()), Status::Ok, "and the next poll takes it");
    }

    /// Shared with a timer handler. The handler re-arms itself until it sees
    /// the test parked, so the post below always lands on a waiter rather
    /// than on the count, however slow the emulated CPU is.
    struct IrqCase {
        sync::Semaphore sema;
        usize waiters_at_post = 0;
        usize rearms = 0;
    };

    /// Enough re-arms, POLL_INTERVAL apart, to outlast the test's LOST_AFTER:
    /// the handler gives up and posts after that anyway.
    constexpr usize MAX_REARMS = 5000;

    KTEST(Semaphore, PostFromTimerHandlerWakesAParkedWaiter) {
        IrqCase c;
        time::Timer timer;
        auto *const pc = &c;
        timer.activate(time::Deadline::at(time::SteadyClock::now() + POLL_INTERVAL),
                       [pc](time::Timer *const self) {
                           // In the timer interrupt: interrupts off, on the
                           // CPU that armed the timer.
                           if (pc->sema.num_waiters() == 0 && pc->rearms < MAX_REARMS) {
                               pc->rearms += 1;
                               auto _ = self->forward(POLL_INTERVAL);
                               return time::Timer::HandlerResult::Restart;
                           }
                           pc->waiters_at_post = pc->sema.num_waiters();
                           pc->sema.post();
                           return time::Timer::HandlerResult::Handled;
                       });

        auto const status = c.sema.wait(lost_after_from_now());
        // Waits for a handler still running; after it, |c| is the test's.
        auto _ = timer.cancel();

        EXPECT_EQ(status, Status::Ok, "a post from a timer interrupt wakes a parked waiter");
        EXPECT_EQ(c.waiters_at_post, usize{1},
                  "the waiter was parked when the interrupt posted after {} re-arms", c.rearms);
        EXPECT_EQ(c.sema.count(), u64{0}, "and the unit went to it, not to the count");
    }

    /// Shared between the test and CROWD waiters. On the heap, as Handoff is;
    /// each waiter's wait ends by LOST_AFTER at the latest, so the joins do
    /// not hang.
    struct Crowd {
        sync::Semaphore sema;
        time::Deadline deadline;
        std::atomic<usize> woken{0};
        std::atomic<usize> failed{0};
    };

    auto crowd_waiter(void *const arg) -> i32 {
        auto &c = *static_cast<Crowd *>(arg);
        if (c.sema.wait(c.deadline) == Status::Ok) {
            c.woken.fetch_add(1, std::memory_order_acq_rel);
        } else {
            c.failed.fetch_add(1, std::memory_order_acq_rel);
        }
        return 0;
    }

    KTEST(Semaphore, EachPostWakesExactlyOne) {
        auto *const crowd = new (mm::GAF_KERNEL) Crowd{};
        ASSERT_NONNULL(crowd, "allocate the crowd case");
        auto &c = *crowd;
        c.deadline = lost_after_from_now();

        std::array<Thread *, CROWD> waiters{};
        auto spawned = true;
        for (auto &waiter: waiters) {
            waiter = start("semaphore-test-waiter", crowd_waiter, crowd);
            spawned &= waiter != nullptr;
        }
        EXPECT_TRUE(spawned, "spawn the waiters");

        auto const parked = poll_until([&c] { return c.sema.num_waiters() == CROWD; });
        EXPECT_TRUE(parked, "every waiter parks");

        for (usize i = 0; i + 1 < CROWD; ++i) {
            c.sema.post();
        }
        auto const woke =
            poll_until([&c] { return c.woken.load(std::memory_order_acquire) == CROWD - 1; });
        auto _ = Thread::Current::sleep_for(SETTLE, task::Interruptible::No);
        EXPECT_TRUE(woke && c.woken.load(std::memory_order_acquire) == CROWD - 1 &&
                        c.sema.num_waiters() == 1,
                    "each of the posts but one woke one waiter, and one waiter is left");

        c.sema.post();
        auto joined = true;
        for (auto *const waiter: waiters) {
            if (waiter != nullptr) {
                joined &= join(waiter);
            }
        }
        if (!joined) {
            FAIL("join the waiters; leaking their state");
        }
        EXPECT_TRUE(c.woken.load(std::memory_order_acquire) == CROWD &&
                        c.failed.load(std::memory_order_acquire) == 0 && c.sema.count() == 0,
                    "the last post woke the last waiter, and nothing was banked");
        delete crowd;
    }

} // namespace
} // namespace ours
