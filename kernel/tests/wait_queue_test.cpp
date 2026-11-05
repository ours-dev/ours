/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// WaitQueue.WakesByTimeoutAndSuspend: wake a parked thread the two ways that
// pick one particular thread out of a wait queue, rather than whoever is at
// its head, and check that the waiter comes back, with the status that says
// why:
//
//   - its own timeout: a wait with a deadline that nothing signals, which
//     WaitQueue::handle_timeout() ends from the timer interrupt, and which
//     answers TimeOut;
//   - a suspend from another thread while it waits interruptibly, which
//     Thread::suspend_or_kill() ends through its Parked branch, and which
//     answers InternalIrqRetry -- not Ok, which would tell the waiter it got
//     what it waited for.
//
// Both reach WaitQueue::unpark_thread() holding the woken thread's PI lock,
// and until 2026-10-03 that function took the same lock again: dbg_1 would
// stop on SpinLock::acquire()'s !is_held() assertion, and a build without
// assertions would spin for good with interrupts off. Nothing in a normal
// boot parks with a deadline or suspends a parked thread -- object_wait_one
// refuses finite deadlines and no system call suspends -- which is how it
// stayed unseen. This makes both happen.
//
// Why an in-kernel test: a wait queue's slow path is the scheduler, so a host
// test can neither park a thread nor end its park from a timer interrupt, and
// the suspend path needs another thread to run while this one is parked.
#include <ours/lockdep.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/sync/event.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>

#include <heap/new.hpp>
#include <ktest/ktest.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    /// How long a wake may take before the test calls it lost. Generous for
    /// the reason mutex.stress's STUCK_AFTER is: under QEMU's TCG every park
    /// and wake is a pair of context switches on emulated CPUs.
    constexpr auto LOST_AFTER = Seconds(5);
    constexpr auto POLL_INTERVAL = Milliseconds(1);
    constexpr auto TIMEOUT = Milliseconds(5);

    /// Polls |condition| until it holds or LOST_AFTER has passed.
    template <typename Condition>
    auto poll_until(Condition const &condition) -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (!condition()) {
            if (time::SteadyClock::now() >= give_up) {
                return false;
            }
            auto _ = task::Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }
        return true;
    }

    /// A wait nobody ends but its own deadline.
    auto check_timeout() -> void {
        sync::Event event;
        auto const started = time::SteadyClock::now();
        auto const status = event.wait(time::Deadline::at(started + TIMEOUT));
        auto const elapsed = time::SteadyClock::now() - started;

        log::info("WaitQueue:   timed wait answered {} after {} us", to_string(status),
                  duration_cast<Microseconds>(elapsed).count());
        EXPECT_EQ(status, Status::TimeOut, "a wait that nothing signals times out");
        EXPECT_TRUE(elapsed >= TIMEOUT, "and not before its deadline");
    }

    /// Shared between the test and the waiter it suspends. On the heap: a
    /// waiter that is never woken still points at it after the test has given
    /// up, so on that path it is leaked rather than popped.
    struct SuspendCase {
        /// Never signalled while the test passes: the suspend is what ends
        /// the waiter's wait on it.
        sync::Event wait_here;
        /// Ends the waiter's second wait, once the test has resumed it.
        sync::Event release;
        /// Written by the waiter before |woken|, read after it.
        Status woken_status = Status::Ok;
        std::atomic<bool> woken{false};
        std::atomic<bool> done{false};
    };

    auto suspended_waiter(void *arg) -> i32 {
        auto &c = *static_cast<SuspendCase *>(arg);

        c.woken_status = c.wait_here.wait(time::Deadline::infinite());
        c.woken.store(true, std::memory_order_release);

        // Not interruptible: the Suspend signal is still pending -- a kernel
        // thread never reaches process_pending_signals() to act on it -- and
        // an interruptible wait would return at once on it.
        auto _ = c.release.wait();

        // Last touch of |c|: once the test sees this, it frees it.
        c.done.store(true, std::memory_order_release);
        return 0;
    }

    /// Whether |thread| is parked, interruptibly, in a wait queue. Read
    /// under its PI lock, which is what suspend_or_kill() decides by.
    auto is_parked_interruptibly(task::Thread *const thread) -> bool {
        Guard<sync::SpinLock, lockdep::IrqSave> const guard{thread->get_pi_lock()};
        auto &waiter = thread->waiter_state();
        return thread->is_state_parked() && waiter.wait_queue() != nullptr &&
               waiter.is_interruptible();
    }

    KTEST(WaitQueue, WakesByTimeoutAndSuspend) {
        check_timeout();

        auto *const c = new (mm::ALLOC_KERNEL) SuspendCase{};
        ASSERT_NONNULL(c, "allocate the suspend case");

        auto *const waiter =
            task::Thread::spawn("waitqueue-test", suspended_waiter, c, task::DEFAULT_PRIORITY);
        if (waiter == nullptr) {
            delete c;
            FAIL("spawn the waiter");
        }
        // detach() and resume() separately: detach_and_resume() is declared
        // but has no definition yet.
        auto _ = waiter->detach();
        auto _ = waiter->resume();

        // |waiter| is alive from here until it sets |done|: it parks on
        // |wait_here| and then on |release|, and nothing else ends it.
        if (!poll_until([waiter] { return is_parked_interruptibly(waiter); })) {
            // It never parked, so it may yet: signal both, and leak |c|.
            c->wait_here.signal();
            c->release.signal();
            FAIL("the waiter parks interruptibly; leaking its state");
        }

        auto _ = waiter->suspend();
        auto const woken = poll_until([c] { return c->woken.load(std::memory_order_acquire); });
        EXPECT_TRUE(woken, "suspending a parked interruptible waiter wakes it");
        if (!woken) {
            // Release it the ordinary way, so it is not left parked for good.
            c->wait_here.signal();
            if (!poll_until([c] { return c->woken.load(std::memory_order_acquire); })) {
                log::error("WaitQueue:   the waiter is lost; leaking its state");
                return;
            }
        }
        log::info("WaitQueue:   the suspended wait answered {}", to_string(c->woken_status));
        EXPECT_EQ(c->woken_status, Status::InternalIrqRetry,
                  "the suspended wait answers InternalIrqRetry, not Ok");

        // The other half of the protocol: drop the Suspend signal, then let
        // the waiter finish.
        auto _ = waiter->resume();
        c->release.signal();
        if (!poll_until([c] { return c->done.load(std::memory_order_acquire); })) {
            log::error("WaitQueue:   the waiter did not finish; leaking its state");
            FAIL("the waiter finishes once released");
        }
        delete c;
    }

} // namespace
} // namespace ours
