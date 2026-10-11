/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `semaphore.test`: sync::Semaphore, the counting semaphore a port's waiters
// sleep on (.agents/tasks/microkernel-interrupts/PLAN.md, step 3). Its one
// invariant is that a thread waits only while the count is zero: a post hands
// its unit to a waiter if there is one and banks it otherwise, and a wait
// takes a banked unit if there is one and blocks otherwise. Each check below
// looks at one side of that:
//
//   - posts with nobody waiting are banked: as many polls as posts take a
//     unit each without blocking, and the next poll answers TimeOut;
//   - a wait with nothing banked blocks until another thread posts, and the
//     post hands its unit over rather than banking it;
//   - a wait with a deadline times out and leaves the queue, so a post after
//     it is banked instead of going to a waiter that has gone;
//   - a post from a timer handler, in interrupt context, wakes a parked
//     waiter: what an interrupt object will do to a port;
//   - with several waiters, each post wakes exactly one.
//
// A console command rather than a host test for the reason mutex.stress
// gives: a semaphore's slow path is the scheduler.
#include <ours/mm/gaf.hpp>
#include <ours/sync/semaphore.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>

#include <array>
#include <atomic>

#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

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

    /// Units banked by the first check.
    constexpr usize BANKED = 8;
    /// Threads waiting at once in the crowd check.
    constexpr usize CROWD = 4;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("semaphore.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("semaphore.test: FAIL  {}", name);
    }

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

    auto check_banked(Tally &tally) -> void {
        sync::Semaphore sema;
        for (usize i = 0; i < BANKED; ++i) {
            sema.post();
        }
        verdict(tally, sema.count() == BANKED, "posts with nobody waiting are banked");

        usize taken = 0;
        while (taken < BANKED && sema.wait(already_passed()) == Status::Ok) {
            taken += 1;
        }
        verdict(tally, taken == BANKED, "as many polls as posts take a unit each");
        verdict(tally, sema.wait(already_passed()) == Status::TimeOut,
                "the next poll answers TimeOut");
    }

    /// Shared between the command, which waits, and the thread that posts
    /// once it has seen the command parked. On the heap, and freed only once
    /// the poster is joined: if the join fails, it is leaked rather than
    /// freed under a thread that may still use it.
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

    auto check_handoff(Tally &tally) -> void {
        auto *const h = new (mm::GAF_KERNEL) Handoff{};
        if (h == nullptr) {
            verdict(tally, false, "allocate the handoff case");
            return;
        }
        auto *const poster = start("semaphore-test-poster", handoff_poster, h);
        if (poster == nullptr) {
            verdict(tally, false, "spawn the poster");
            delete h;
            return;
        }

        auto const status = h->sema.wait(lost_after_from_now());
        if (!join(poster)) {
            verdict(tally, false, "join the poster; leaking its state");
            return;
        }

        verdict(tally, status == Status::Ok, "a wait with nothing banked returns once posted");
        verdict(tally, h->saw_waiter.load(std::memory_order_acquire),
                "and it was parked until the post");
        verdict(tally, h->sema.count() == 0,
                "the post handed its unit over rather than banking it");
        delete h;
    }

    auto check_timeout(Tally &tally) -> void {
        sync::Semaphore sema;
        auto const started = time::SteadyClock::now();
        auto const status = sema.wait(time::Deadline::at(started + TIMEOUT));
        auto const elapsed = time::SteadyClock::now() - started;

        log::info("semaphore.test:   timed wait answered {} after {} us", to_string(status),
                  duration_cast<Microseconds>(elapsed).count());
        verdict(tally, status == Status::TimeOut, "a wait that nothing posts times out");
        verdict(tally, elapsed >= TIMEOUT, "and not before its deadline");
        verdict(tally, sema.num_waiters() == 0, "the timed-out waiter has left the queue");

        sema.post();
        verdict(tally, sema.count() == 1, "so a post after it is banked, not lost on it");
        verdict(tally, sema.wait(already_passed()) == Status::Ok, "and the next poll takes it");
    }

    /// Shared with a timer handler. The handler re-arms itself until it sees
    /// the command parked, so the post below always lands on a waiter rather
    /// than on the count, however slow the emulated CPU is.
    struct IrqCase {
        sync::Semaphore sema;
        usize waiters_at_post = 0;
        usize rearms = 0;
    };

    /// Enough re-arms, POLL_INTERVAL apart, to outlast the command's
    /// LOST_AFTER: the handler gives up and posts after that anyway.
    constexpr usize MAX_REARMS = 5000;

    auto check_post_from_interrupt(Tally &tally) -> void {
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
        // Waits for a handler still running; after it, |c| is the command's.
        auto _ = timer.cancel();

        log::info("semaphore.test:   the timer re-armed {} time(s) before it saw the waiter",
                  c.rearms);
        verdict(tally, status == Status::Ok, "a post from a timer interrupt wakes a parked waiter");
        verdict(tally, c.waiters_at_post == 1, "the waiter was parked when the interrupt posted");
        verdict(tally, c.sema.count() == 0, "and the unit went to it, not to the count");
    }

    /// Shared between the command and CROWD waiters. On the heap, as Handoff
    /// is; each waiter's wait ends by LOST_AFTER at the latest, so the joins
    /// do not hang.
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

    auto check_crowd(Tally &tally) -> void {
        auto *const crowd = new (mm::GAF_KERNEL) Crowd{};
        if (crowd == nullptr) {
            verdict(tally, false, "allocate the crowd case");
            return;
        }
        auto &c = *crowd;
        c.deadline = lost_after_from_now();

        std::array<Thread *, CROWD> waiters{};
        auto spawned = true;
        for (auto &waiter: waiters) {
            waiter = start("semaphore-test-waiter", crowd_waiter, crowd);
            spawned &= waiter != nullptr;
        }
        verdict(tally, spawned, "spawn the waiters");

        auto const parked = poll_until([&c] { return c.sema.num_waiters() == CROWD; });
        verdict(tally, parked, "every waiter parks");

        for (usize i = 0; i + 1 < CROWD; ++i) {
            c.sema.post();
        }
        auto const woke =
            poll_until([&c] { return c.woken.load(std::memory_order_acquire) == CROWD - 1; });
        auto _ = Thread::Current::sleep_for(SETTLE, task::Interruptible::No);
        verdict(tally,
                woke && c.woken.load(std::memory_order_acquire) == CROWD - 1 &&
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
            verdict(tally, false, "join the waiters; leaking their state");
            return;
        }
        verdict(tally,
                c.woken.load(std::memory_order_acquire) == CROWD &&
                    c.failed.load(std::memory_order_acquire) == 0 && c.sema.count() == 0,
                "the last post woke the last waiter, and nothing was banked");
        delete crowd;
    }

    auto semaphore_test_command(int, char **) -> int {
        Tally tally;

        check_banked(tally);
        check_handoff(tally);
        check_timeout(tally);
        check_post_from_interrupt(tally);
        check_crowd(tally);

        if (tally.failed != 0) {
            log::error("semaphore.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("semaphore.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(SemaphoreTest, "semaphore.test", ours::semaphore_test_command,
             "post and wait on a counting semaphore, from threads and from a timer interrupt");
