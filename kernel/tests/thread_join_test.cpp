/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// ThreadJoin.JoinAndDetachFollowTheirContract: Thread::join() and detach()
// against the contract on their declarations
// (kernel/include/ours/task/thread.hpp) and design option B
// (.agents/tasks/thread-join/DESIGN.md).
//
//   - joining a thread that has already exited returns its return code;
//   - joining a thread that is still running waits for it to exit;
//   - a join that reaches its deadline answers TimeOut and gives its claim
//     back, so a second join succeeds;
//   - while a join is in progress, a second join and a detach() both answer
//     BadState;
//   - joining a detached thread answers BadState;
//   - detach() of a thread that has already exited reaps it.
//
// The `stress` run option adds, per round, one joinable thread per CPU, each
// joined at once, so exits race joins on every CPU; `n=<rounds>` says how
// many, 2000 without it, the count the recorded stress runs used. Run it on
// dbg_1, where lockdep and the PI-lock assertions are compiled in.
//
// Each case is written so that a join() that did not reap its thread leaves
// the shared state leaked rather than freed under a thread that may still use
// it: a broken join() shows up as a failed check, not as a use after free.
//
// An in-kernel test rather than a unit test for the reason mutex.stress
// gives: a join's slow path is the scheduler.
#include <ours/cpu.hpp>
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
    using task::Thread;

    constexpr auto LOST_AFTER = Seconds(5);
    constexpr auto POLL_INTERVAL = Milliseconds(1);
    /// How long a thread a join waits for runs before it returns: long enough
    /// that the join is certainly parked by then.
    constexpr auto RUN_FOR = Milliseconds(20);
    /// The deadline of a join that is meant to time out.
    constexpr auto SHORT_WAIT = Milliseconds(10);
    /// The stress round count when a run picked `stress` without `n`: the
    /// count every recorded stress run used (2000 rounds of one thread per
    /// CPU took about five seconds on the dbg image).
    constexpr usize DEFAULT_STRESS_ROUNDS = 2000;

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

    auto deadline_after(Duration const delta) -> time::Deadline {
        return time::Deadline::at(time::SteadyClock::now() + delta);
    }

    /// Whether |thread| is Dead. Read under its PI lock, which guards the
    /// state; |thread| must be one nobody can have freed.
    auto is_dead(Thread *const thread) -> bool {
        Guard<sync::SpinLock, lockdep::IrqSave> const guard{thread->get_pi_lock()};
        return thread->state() == Thread::State::Dead;
    }

    /// Whether |thread| is parked. Same rule as is_dead().
    auto is_parked(Thread *const thread) -> bool {
        Guard<sync::SpinLock, lockdep::IrqSave> const guard{thread->get_pi_lock()};
        return thread->is_state_parked();
    }

    /// What a joinable thread of this test does, in order: optionally wait on
    /// |release|, optionally run for RUN_FOR, record when it returns, and
    /// return |retcode|. On the heap, and freed only once the thread is
    /// known to be reaped -- or leaked.
    struct Target {
        i32 retcode = 0;
        bool wait_for_release = false;
        bool run_a_while = false;
        sync::Event release;
        TimePoint<> returning_at;
        std::atomic<bool> returning{false};
    };

    auto target_routine(void *arg) -> i32 {
        auto &t = *static_cast<Target *>(arg);
        if (t.wait_for_release) {
            auto _ = t.release.wait();
        }
        if (t.run_a_while) {
            auto _ = Thread::Current::sleep_for(RUN_FOR, task::Interruptible::No);
        }
        // Read before |returning| is published: a detached thread's state is
        // freed by the test as soon as it sees that, so nothing may read |t|
        // after it.
        auto const retcode = t.retcode;
        t.returning_at = time::SteadyClock::now();
        t.returning.store(true, std::memory_order_release);
        return retcode;
    }

    /// Spawns a joinable thread running |target|: not detached, resumed.
    auto spawn_joinable(Target *const target, CpuNum const cpu = INVALID_CPU_NUM) -> Thread * {
        auto *const thread =
            Thread::spawn("join-test", target_routine, target, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        if (cpu != INVALID_CPU_NUM) {
            thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        }
        auto _ = thread->resume();
        return thread;
    }

    /// Frees |target| if |reaped|, which says its thread is gone; otherwise
    /// the thread may still use it, and it is leaked.
    auto settle(Target *const target, bool const reaped) -> void {
        if (reaped) {
            delete target;
            return;
        }
        log::error("threadjoin:   the thread was not reaped; leaking its state");
    }

    auto check_join_after_exit() -> void {
        auto *const target = new (mm::ALLOC_KERNEL) Target{};
        target->retcode = 42;
        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            delete target;
            FAIL("spawn a joinable thread");
        }
        auto const exited = poll_until([thread] { return is_dead(thread); });
        EXPECT_TRUE(exited, "the thread exits");
        if (!exited) {
            settle(target, false);
            return;
        }

        i32 retcode = -1;
        auto const status = thread->join(&retcode, time::Deadline::infinite());
        log::info("threadjoin:   join after exit answered {}, retcode {}", to_string(status),
                  retcode);
        EXPECT_TRUE(status == Status::Ok && retcode == 42,
                    "joining a thread that has exited returns its return code");
        settle(target, status == Status::Ok);
    }

    auto check_join_waits() -> void {
        auto *const target = new (mm::ALLOC_KERNEL) Target{};
        target->retcode = 7;
        target->run_a_while = true;
        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            delete target;
            FAIL("spawn a joinable thread");
        }

        i32 retcode = -1;
        auto const status = thread->join(&retcode, time::Deadline::infinite());
        auto const joined_at = time::SteadyClock::now();
        auto const waited = status == Status::Ok &&
                            target->returning.load(std::memory_order_acquire) &&
                            joined_at >= target->returning_at;
        EXPECT_TRUE(waited && retcode == 7,
                    "joining a running thread waits for it to exit, then returns its return code");
        settle(target, status == Status::Ok);
    }

    auto check_join_times_out() -> void {
        auto *const target = new (mm::ALLOC_KERNEL) Target{};
        target->retcode = 3;
        target->wait_for_release = true;
        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            delete target;
            FAIL("spawn a joinable thread");
        }

        i32 retcode = -1;
        auto const started = time::SteadyClock::now();
        auto const first = thread->join(&retcode, deadline_after(SHORT_WAIT));
        auto const elapsed = time::SteadyClock::now() - started;
        EXPECT_TRUE(first == Status::TimeOut && retcode == -1 && elapsed >= SHORT_WAIT,
                    "a join that reaches its deadline answers TimeOut and leaves retcode alone");

        target->release.signal();
        auto const second = thread->join(&retcode, time::Deadline::infinite());
        EXPECT_TRUE(second == Status::Ok && retcode == 3,
                    "and gives its claim back: a second join reaps the thread");
        settle(target, second == Status::Ok);
    }

    /// The joiner of check_one_reaper(): detached, so it frees itself.
    struct Joiner {
        Thread *target_thread = nullptr;
        Status status = Status::Ok;
        i32 retcode = -1;
        std::atomic<bool> joining{false};
        std::atomic<bool> done{false};
    };

    auto joiner_routine(void *arg) -> i32 {
        auto &j = *static_cast<Joiner *>(arg);
        j.joining.store(true, std::memory_order_release);
        j.status = j.target_thread->join(&j.retcode, time::Deadline::infinite());
        // Last touch of |j|: once the test sees this, it frees it.
        j.done.store(true, std::memory_order_release);
        return 0;
    }

    auto check_one_reaper() -> void {
        auto *const target = new (mm::ALLOC_KERNEL) Target{};
        auto *const joiner = new (mm::ALLOC_KERNEL) Joiner{};
        target->retcode = 4;
        target->wait_for_release = true;

        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            delete target;
            delete joiner;
            FAIL("spawn a joinable thread");
        }
        joiner->target_thread = thread;
        auto *const joiner_thread =
            Thread::spawn("join-test-joiner", joiner_routine, joiner, task::DEFAULT_PRIORITY);
        if (joiner_thread == nullptr) {
            target->release.signal();
            settle(target, false);
            delete joiner;
            FAIL("spawn the joiner");
        }
        auto _ = joiner_thread->detach();
        auto _ = joiner_thread->resume();

        // The joiner parks only inside join(), and only once it holds the
        // claim. It is alive while parked there: |thread| has not exited.
        auto const parked = poll_until([joiner, joiner_thread] {
            return joiner->done.load(std::memory_order_acquire) ||
                   (joiner->joining.load(std::memory_order_acquire) && is_parked(joiner_thread));
        });
        auto const claimed = parked && !joiner->done.load(std::memory_order_acquire);
        EXPECT_TRUE(claimed, "the joiner waits in join()");

        if (claimed) {
            i32 retcode = -1;
            auto const second = thread->join(&retcode, deadline_after(SHORT_WAIT));
            EXPECT_TRUE(second == Status::BadState,
                        "a second join while one is in progress answers BadState");
            EXPECT_TRUE(thread->detach() == Status::BadState,
                        "and so does detach(): the joiner holds the claim");
        }

        target->release.signal();
        auto const finished =
            poll_until([joiner] { return joiner->done.load(std::memory_order_acquire); });
        auto const reaped = finished && joiner->status == Status::Ok;
        EXPECT_TRUE(reaped && joiner->retcode == 4, "the joiner reaps the thread");
        settle(target, reaped);
        if (finished) {
            delete joiner;
        }
    }

    auto check_join_of_detached() -> void {
        auto *const target = new (mm::ALLOC_KERNEL) Target{};
        target->wait_for_release = true;
        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            delete target;
            FAIL("spawn a joinable thread");
        }
        auto const detached = thread->detach();
        EXPECT_TRUE(detached == Status::Ok, "a running thread can be detached");

        i32 retcode = -1;
        auto const status = thread->join(&retcode, deadline_after(SHORT_WAIT));
        EXPECT_TRUE(status == Status::BadState, "joining a detached thread answers BadState");

        // Detached, it frees itself once released, and stops reading
        // |target| once it says it is returning.
        target->release.signal();
        auto const returned =
            poll_until([target] { return target->returning.load(std::memory_order_acquire); });
        settle(target, returned);
    }

    auto check_detach_after_exit() -> void {
        auto *const target = new (mm::ALLOC_KERNEL) Target{};
        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            delete target;
            FAIL("spawn a joinable thread");
        }
        auto const exited = poll_until([thread] { return is_dead(thread); });
        EXPECT_TRUE(exited, "the thread exits");
        if (!exited) {
            settle(target, false);
            return;
        }
        auto const status = thread->detach();
        EXPECT_TRUE(status == Status::Ok, "detach() of a thread that has exited reaps it");
        settle(target, status == Status::Ok);
    }

    /// Per round, one joinable thread per CPU, joined in spawn order as soon
    /// as all are spawned: the early ones have exited, the late ones are still
    /// starting, so both join paths run, and exits race joins on every CPU.
    auto check_stress(usize const rounds) -> void {
        auto const num_cpus = Cpu::num_online_cpus();
        constexpr usize MAX_CPUS = 64;
        auto width = num_cpus;
        if (width > MAX_CPUS) {
            width = MAX_CPUS;
        }

        usize bad = 0;
        auto const started = time::SteadyClock::now();
        for (usize round = 0; round < rounds && bad == 0; ++round) {
            Target *targets[MAX_CPUS] = {};
            Thread *threads[MAX_CPUS] = {};
            for (usize i = 0; i < width; ++i) {
                targets[i] = new (mm::ALLOC_KERNEL) Target{};
                targets[i]->retcode = static_cast<i32>(round * width + i);
                threads[i] = spawn_joinable(targets[i], static_cast<CpuNum>(i));
            }
            for (usize i = 0; i < width; ++i) {
                if (threads[i] == nullptr) {
                    bad += 1;
                    delete targets[i];
                    continue;
                }
                i32 retcode = -1;
                auto const status = threads[i]->join(&retcode, time::Deadline::infinite());
                if (status != Status::Ok || retcode != targets[i]->retcode) {
                    bad += 1;
                }
                settle(targets[i], status == Status::Ok);
            }
        }
        auto const elapsed = time::SteadyClock::now() - started;
        log::info("threadjoin:   stress: {} round(s) x {} thread(s) in {} ms, {} bad", rounds,
                  width, duration_cast<Milliseconds>(elapsed).count(), bad);
        EXPECT_TRUE(bad == 0, "every stressed join reaps its thread with its return code");
    }

    KTEST(ThreadJoin, JoinAndDetachFollowTheirContract) {
        check_join_after_exit();
        check_join_waits();
        check_join_times_out();
        check_one_reaper();
        check_join_of_detached();
        check_detach_after_exit();
        // The old command ran the stress rounds only when the console line
        // said `stress ROUNDS`; the option keeps that, with `n` for the round
        // count.
        if (ktest::option_usize("stress", 0) != 0) {
            check_stress(ktest::option_usize("n", DEFAULT_STRESS_ROUNDS));
        }
    }
} // namespace
} // namespace ours
