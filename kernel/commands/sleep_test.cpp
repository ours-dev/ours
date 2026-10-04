/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `sleep.test`: what becomes of a thread in Thread::Current::sleep_etc() when
// a suspend reaches it before its deadline.
//
//   - An interruptible sleeper wakes early and reads InternalIrqRetry, not
//     Ok, which would say it slept its full time.
//   - Its timer, still armed after the early wake, must leave it alone when
//     it fires. The sleeper cancels the timer only once it runs again, and
//     until then the timer can fire on a thread that is already Ready; it
//     used to unsleep it a second time.
//   - A sleeper that is not interruptible is not woken at all: it sleeps to
//     its deadline and reads Ok.
//
// The timer's window is the time between the early wake and the woken
// thread reaching timer.cancel(), and in an ordinary run it is too short to
// land a timer in. So the test holds it open: the sleeper is pinned to one
// CPU, and a "hog" thread pinned to the same CPU spins there with preemption
// disabled from before the suspend until after the deadline. The woken
// sleeper waits on that CPU's run queue, unable to run, while its timer fires
// in the hog's interrupts.
//
// A console command rather than a unit test for the reason mutex.stress
// gives: a sleep's slow path is the scheduler.
#include <ours/cpu.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/sync/event.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>

#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    /// How long a sleeper sleeps. Long against everything the command does
    /// before its suspend -- spawning threads and seeing each one settle,
    /// under QEMU's TCG -- so that the suspend lands well before the deadline.
    constexpr auto SLEEP = Milliseconds(100);
    /// How long past the deadline the hog keeps the CPU: the timer has to
    /// have fired, on a tick, before the sleeper may run.
    constexpr auto HOLD_PAST_DEADLINE = Milliseconds(20);
    constexpr auto LOST_AFTER = Seconds(5);
    constexpr auto POLL_INTERVAL = Milliseconds(1);

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("sleep.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("sleep.test: FAIL  {}", name);
    }

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

    /// Shared between the command, the sleeper and, when there is one, the
    /// hog. On the heap: on a failure path the threads still point at it
    /// after the command has given up, and it is leaked rather than popped.
    struct SleepCase {
        explicit SleepCase(task::Interruptible const interruptible)
            : interruptible(interruptible) {}

        task::Interruptible const interruptible;
        /// Set by the sleeper before it sleeps, read by the others.
        TimePoint<> deadline;
        /// Written by the sleeper before |woken|, read after it.
        Status woken_status = Status::Ok;
        TimePoint<> woken_at;
        std::atomic<bool> sleeping{false};
        std::atomic<bool> woken{false};
        /// Ends the sleeper's last wait, once the command has resumed it.
        sync::Event release;
        std::atomic<bool> sleeper_done{false};

        std::atomic<bool> hogging{false};
        /// True from the start for a case without a hog.
        std::atomic<bool> hog_done{false};
    };

    auto sleeper(void *arg) -> i32 {
        auto &c = *static_cast<SleepCase *>(arg);

        auto const now = time::SteadyClock::now();
        c.deadline = now + SLEEP;
        c.sleeping.store(true, std::memory_order_release);
        c.woken_status =
            task::Thread::Current::sleep_etc(time::Deadline::at(c.deadline), c.interruptible, now);
        c.woken_at = time::SteadyClock::now();
        c.woken.store(true, std::memory_order_release);

        // Not interruptible: the Suspend signal is still pending -- a kernel
        // thread never reaches process_pending_signals() to act on it -- and
        // an interruptible wait would return at once on it.
        auto _ = c.release.wait();

        // Last touch of |c| by this thread.
        c.sleeper_done.store(true, std::memory_order_release);
        return 0;
    }

    /// Keeps its CPU, preemption off, until the sleeper's timer has fired.
    auto hog(void *arg) -> i32 {
        auto &c = *static_cast<SleepCase *>(arg);
        {
            task::AutoPreemptDisabler const no_preemption;
            // Published with preemption already off: once the command sees
            // it, nothing else can run on this CPU, the woken sleeper included.
            c.hogging.store(true, std::memory_order_release);

            auto const until = c.deadline + HOLD_PAST_DEADLINE;
            while (time::SteadyClock::now() < until) {
                arch::spin_hint();
            }
        }

        // Last touch of |c| by this thread.
        c.hog_done.store(true, std::memory_order_release);
        return 0;
    }

    /// Whether |thread| is in sleep_etc(), asleep. Read under its PI lock,
    /// which is what suspend_or_kill() decides by.
    auto is_asleep(task::Thread *const thread) -> bool {
        Guard<sync::SpinLock, lockdep::IrqSave> const guard{thread->get_pi_lock()};
        return thread->is_state_sleeping();
    }

    auto spawn_on(char const *const name, task::Thread::StartRoutine routine, void *const arg,
                  CpuNum const cpu) -> task::Thread * {
        auto *const thread = task::Thread::spawn(name, routine, arg, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        // detach() and resume() separately: detach_and_resume() is declared
        // but has no definition yet.
        auto _ = thread->detach();
        auto _ = thread->resume();
        return thread;
    }

    /// Spawns |c|'s sleeper on |cpu| and waits until it is asleep. Null if it
    /// did not get there; |c| is then the sleeper's, or freed if it never ran.
    auto start_sleeper(Tally &tally, SleepCase *const c, CpuNum const cpu) -> task::Thread * {
        auto *const thread = spawn_on("sleep-test", sleeper, c, cpu);
        if (thread == nullptr) {
            verdict(tally, false, "spawn the sleeper");
            delete c;
            return nullptr;
        }
        // |thread| is alive until it sets |sleeper_done|, which it does only
        // after the command signals |release|.
        auto const asleep = poll_until([c, thread] {
            return c->sleeping.load(std::memory_order_acquire) && is_asleep(thread);
        });
        verdict(tally, asleep, "the sleeper is asleep");
        if (!asleep) {
            log::error("sleep.test:   leaking the case; the sleeper may still use it");
            return nullptr;
        }
        return thread;
    }

    /// Waits for |c|'s sleeper to wake, resumes it and lets it finish. False
    /// if it did not; |c| is then leaked, and freed otherwise.
    auto finish_sleeper(Tally &tally, SleepCase *const c, task::Thread *const thread) -> bool {
        auto const woken = poll_until([c] { return c->woken.load(std::memory_order_acquire); });
        verdict(tally, woken, "the sleeper runs again");
        if (!woken) {
            log::error("sleep.test:   leaking the case; the sleeper may still use it");
            return false;
        }

        // The other half of the protocol: drop the Suspend signal, then let
        // the sleeper finish.
        auto _ = thread->resume();
        c->release.signal();
        auto const finished = poll_until([c] {
            return c->sleeper_done.load(std::memory_order_acquire) &&
                   c->hog_done.load(std::memory_order_acquire);
        });
        verdict(tally, finished, "its threads finish");
        if (!finished) {
            log::error("sleep.test:   leaking the case; a thread may still use it");
            return false;
        }
        return true;
    }

    auto check_early_wake(Tally &tally) -> void {
        auto const num_cpus = Cpu::num_online_cpus();
        if (num_cpus < 2) {
            verdict(tally, false, "needs a second CPU for the sleeper and the hog");
            return;
        }
        // Any CPU but the command's own would do; the hog's spinning keeps
        // the command off it anyway once it starts.
        auto const cpu = static_cast<CpuNum>(num_cpus - 1);

        auto *const c = new (mm::GAF_KERNEL) SleepCase{task::Interruptible::Yes};
        if (c == nullptr) {
            verdict(tally, false, "allocate the early-wake case");
            return;
        }
        auto *const thread = start_sleeper(tally, c, cpu);
        if (thread == nullptr) {
            return;
        }

        if (spawn_on("sleep-test-hog", hog, c, cpu) == nullptr) {
            verdict(tally, false, "spawn the hog");
            log::error("sleep.test:   leaking the case; the sleeper still uses it");
            return;
        }
        if (!poll_until([c] { return c->hogging.load(std::memory_order_acquire); })) {
            verdict(tally, false, "the hog takes the sleeper's CPU");
            log::error("sleep.test:   leaking the case; the sleeper still uses it");
            return;
        }

        auto const suspended_at = time::SteadyClock::now();
        auto _ = thread->suspend();
        verdict(tally, suspended_at < c->deadline,
                "the suspend lands before the deadline (otherwise nothing was tested)");

        // The sleeper is now Ready on the hog's CPU, its timer still armed:
        // the timer fires there at the deadline, and the sleeper runs only
        // once the hog lets go.
        if (!finish_sleeper(tally, c, thread)) {
            return;
        }
        log::info("sleep.test:   the early-woken sleep answered {}, {} us before its deadline",
                  to_string(c->woken_status),
                  duration_cast<Microseconds>(c->deadline - suspended_at).count());
        verdict(tally, c->woken_status == Status::InternalIrqRetry,
                "an interruptible sleep a suspend ends answers InternalIrqRetry, not Ok");
        delete c;
    }

    auto check_uninterruptible_sleep(Tally &tally) -> void {
        auto *const c = new (mm::GAF_KERNEL) SleepCase{task::Interruptible::No};
        if (c == nullptr) {
            verdict(tally, false, "allocate the uninterruptible case");
            return;
        }
        c->hog_done.store(true, std::memory_order_relaxed);

        auto *const thread = start_sleeper(tally, c, Cpu::id());
        if (thread == nullptr) {
            return;
        }

        auto _ = thread->suspend();
        if (!finish_sleeper(tally, c, thread)) {
            return;
        }
        log::info("sleep.test:   the uninterruptible sleep answered {}, {} us past its deadline",
                  to_string(c->woken_status),
                  duration_cast<Microseconds>(c->woken_at - c->deadline).count());
        verdict(tally, c->woken_at >= c->deadline,
                "a suspend does not end a sleep that is not interruptible");
        verdict(tally, c->woken_status == Status::Ok, "and that sleep answers Ok");
        delete c;
    }

    auto sleep_test_command(int, char **) -> int {
        Tally tally;

        check_early_wake(tally);
        check_uninterruptible_sleep(tally);

        if (tally.failed != 0) {
            log::error("sleep.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("sleep.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(SleepTest, "sleep.test", ours::sleep_test_command,
             "wake a sleeping thread early and check what it reads, and its timer");
