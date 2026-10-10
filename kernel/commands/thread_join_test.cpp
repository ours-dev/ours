/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `threadjoin.test [stress ROUNDS]`: a thread's JoinHandle against the
// contract on its declaration (kernel/include/ours/task/thread.hpp) and the
// design in .agents/tasks/thread-handle/DESIGN.md.
//
//   - joining a thread that has already exited returns its return code and
//     empties the handle;
//   - joining a thread that is still running waits for it to exit;
//   - a join that reaches its deadline answers TimeOut and keeps the
//     thread, so a second join succeeds;
//   - a handle moved to another thread joins there;
//   - detaching a running thread lets it run on and free itself;
//   - detaching a thread that has already exited frees it;
//   - cancelling a thread that was never started frees it unrun;
//   - a thread whose start was deferred is freed unrun if its handle goes
//     first, and runs, to be joined, once resumed through it.
//
// `stress ROUNDS` adds, per round, one thread per CPU, every other one joined
// and the rest detached at once, so exits race joins and handle drops on
// every CPU. Run it on dbg_1, where lockdep and the PI-lock assertions are
// compiled in.
//
// Each case is written so that a thread that may still use its shared state
// leaves it leaked rather than freed: a broken join shows up as a failed
// check, not as a use after free.
//
// A console command rather than a unit test for the reason mutex.stress
// gives: a join's slow path is the scheduler.
#include <ours/cpu.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/sync/event.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>
#include <string_view>

#include <gktl/command.hpp>
#include <heap/new.hpp>
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

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("threadjoin.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("threadjoin.test: FAIL  {}", name);
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

    auto deadline_after(Duration const delta) -> time::Deadline {
        return time::Deadline::at(time::SteadyClock::now() + delta);
    }

    /// Whether |thread| is Dead. Read under its PI lock, which guards the
    /// state; the caller still holds |thread|'s handle.
    auto is_dead(Thread *const thread) -> bool {
        Guard<sync::SpinLock, lockdep::IrqSave> const guard{thread->get_pi_lock()};
        return thread->state() == Thread::State::Dead;
    }

    /// Whether |thread| is parked. Same rule as is_dead().
    auto is_parked(Thread *const thread) -> bool {
        Guard<sync::SpinLock, lockdep::IrqSave> const guard{thread->get_pi_lock()};
        return thread->is_state_parked();
    }

    /// What a thread of this test does, in order: optionally wait on
    /// |release|, optionally run for RUN_FOR, record when it returns, and
    /// return |retcode|. On the heap, and freed only once the thread is known
    /// to be done with it -- or leaked.
    struct Target {
        i32 retcode = 0;
        bool wait_for_release = false;
        bool run_a_while = false;
        sync::Event release;
        TimePoint<> returning_at;
        std::atomic<bool> ran{false};
        std::atomic<bool> returning{false};
    };

    auto target_routine(void *arg) -> i32 {
        auto &t = *static_cast<Target *>(arg);
        t.ran.store(true, std::memory_order_release);
        if (t.wait_for_release) {
            auto _ = t.release.wait();
        }
        if (t.run_a_while) {
            auto _ = Thread::Current::sleep_for(RUN_FOR, task::Interruptible::No);
        }
        // Read before |returning| is published: a detached thread's state is
        // freed by the command as soon as it sees that, so nothing may read
        // |t| after it.
        auto const retcode = t.retcode;
        t.returning_at = time::SteadyClock::now();
        t.returning.store(true, std::memory_order_release);
        return retcode;
    }

    /// Spawns a thread running |target|, unstarted.
    auto spawn_unstarted(Target *const target) -> task::UnstartedThread {
        return Thread::spawn("join-test", target_routine, target, task::DEFAULT_PRIORITY);
    }

    /// Spawns a thread running |target| and starts it.
    auto spawn_target(Target *const target, CpuNum const cpu = INVALID_CPU_NUM)
        -> task::JoinHandle {
        auto thread = spawn_unstarted(target);
        if (!thread) {
            return {};
        }
        if (cpu != INVALID_CPU_NUM) {
            thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        }
        return ustl::move(thread).start();
    }

    /// Frees |target| if |done|, which says its thread no longer uses it;
    /// otherwise the thread may still, and it is leaked.
    auto settle(Target *const target, bool const done) -> void {
        if (done) {
            delete target;
            return;
        }
        log::error("threadjoin.test:   the thread may still use its state; leaking it");
    }

    auto check_join_after_exit(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        target->retcode = 42;
        auto thread = spawn_target(target);
        if (!thread) {
            verdict(tally, false, "spawn a thread");
            delete target;
            return;
        }
        if (!poll_until([&thread] { return is_dead(thread.get()); })) {
            verdict(tally, false, "the thread exits");
            settle(target, false);
            return;
        }

        i32 retcode = -1;
        auto const status = thread.join(&retcode, time::Deadline::infinite());
        log::info("threadjoin.test:   join after exit answered {}, retcode {}", to_string(status),
                  retcode);
        verdict(tally, status == Status::Ok && retcode == 42,
                "joining a thread that has exited returns its return code");
        verdict(tally, !thread, "and leaves the handle empty");
        settle(target, status == Status::Ok);
    }

    auto check_join_waits(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        target->retcode = 7;
        target->run_a_while = true;
        auto thread = spawn_target(target);
        if (!thread) {
            verdict(tally, false, "spawn a thread");
            delete target;
            return;
        }

        i32 retcode = -1;
        auto const status = thread.join(&retcode, time::Deadline::infinite());
        auto const joined_at = time::SteadyClock::now();
        auto const waited = status == Status::Ok &&
                            target->returning.load(std::memory_order_acquire) &&
                            joined_at >= target->returning_at;
        verdict(tally, waited && retcode == 7,
                "joining a running thread waits for it to exit, then returns its return code");
        settle(target, status == Status::Ok);
    }

    auto check_join_times_out(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        target->retcode = 3;
        target->wait_for_release = true;
        auto thread = spawn_target(target);
        if (!thread) {
            verdict(tally, false, "spawn a thread");
            delete target;
            return;
        }

        i32 retcode = -1;
        auto const started = time::SteadyClock::now();
        auto const first = thread.join(&retcode, deadline_after(SHORT_WAIT));
        auto const elapsed = time::SteadyClock::now() - started;
        verdict(tally, first == Status::TimeOut && retcode == -1 && elapsed >= SHORT_WAIT,
                "a join that reaches its deadline answers TimeOut and leaves retcode alone");
        verdict(tally, static_cast<bool>(thread), "and the handle keeps the thread");

        target->release.signal();
        auto const second = thread.join(&retcode, time::Deadline::infinite());
        verdict(tally, second == Status::Ok && retcode == 3, "so a second join succeeds");
        settle(target, second == Status::Ok);
    }

    /// The joiner of check_moved_handle(): it owns the target's handle, moved
    /// in, and joins it.
    struct Joiner {
        task::JoinHandle target_thread;
        Status status = Status::Ok;
        i32 retcode = -1;
        std::atomic<bool> joining{false};
        std::atomic<bool> done{false};
    };

    auto joiner_routine(void *arg) -> i32 {
        auto &j = *static_cast<Joiner *>(arg);
        j.joining.store(true, std::memory_order_release);
        j.status = j.target_thread.join(&j.retcode, time::Deadline::infinite());
        // Last touch of |j|: once the command sees this, it frees it.
        j.done.store(true, std::memory_order_release);
        return 0;
    }

    auto check_moved_handle(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        auto *const joiner = new (mm::GAF_KERNEL) Joiner{};
        target->retcode = 4;
        target->wait_for_release = true;

        joiner->target_thread = spawn_target(target);
        if (!joiner->target_thread) {
            verdict(tally, false, "spawn a thread");
            delete target;
            delete joiner;
            return;
        }
        auto joiner_thread =
            Thread::spawn("join-test-joiner", joiner_routine, joiner, task::DEFAULT_PRIORITY)
                .start();
        if (!joiner_thread) {
            verdict(tally, false, "spawn the joiner");
            target->release.signal();
            // The target's handle goes with |joiner|: it is detached, runs on
            // once released, and is done with |target| once it says so.
            delete joiner;
            settle(target, poll_until([target] {
                       return target->returning.load(std::memory_order_acquire);
                   }));
            return;
        }

        // The joiner parks only inside join(): the target has not exited.
        auto const parked = poll_until([joiner, &joiner_thread] {
            return joiner->done.load(std::memory_order_acquire) ||
                   (joiner->joining.load(std::memory_order_acquire) &&
                    is_parked(joiner_thread.get()));
        });
        verdict(tally, parked && !joiner->done.load(std::memory_order_acquire),
                "the joiner waits in join()");

        target->release.signal();
        i32 joiner_retcode = -1;
        auto const joined = joiner_thread.join(&joiner_retcode, deadline_after(LOST_AFTER));
        auto const reaped = joined == Status::Ok && joiner->status == Status::Ok;
        verdict(tally, reaped && joiner->retcode == 4,
                "a handle moved to another thread joins there");
        settle(target, reaped);
        if (joined == Status::Ok) {
            delete joiner;
        }
    }

    auto check_detach_running(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        target->wait_for_release = true;
        auto thread = spawn_target(target);
        if (!thread) {
            verdict(tally, false, "spawn a thread");
            delete target;
            return;
        }
        thread.detach();
        verdict(tally, !thread, "detaching a running thread empties the handle");

        // Detached, it runs on once released, frees itself after it exits,
        // and stops reading |target| once it says it is returning.
        target->release.signal();
        auto const returned =
            poll_until([target] { return target->returning.load(std::memory_order_acquire); });
        verdict(tally, returned, "and the thread runs on to its end");
        settle(target, returned);
    }

    auto check_detach_after_exit(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        auto thread = spawn_target(target);
        if (!thread) {
            verdict(tally, false, "spawn a thread");
            delete target;
            return;
        }
        if (!poll_until([&thread] { return is_dead(thread.get()); })) {
            verdict(tally, false, "the thread exits");
            settle(target, false);
            return;
        }
        // Frees the thread here, or in its exit Dpc if that has not run yet:
        // either way nothing is left to reap.
        thread.detach();
        verdict(tally, !thread, "detaching a thread that has exited frees it");
        settle(target, true);
    }

    auto check_cancel(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        auto thread = spawn_unstarted(target);
        if (!thread) {
            verdict(tally, false, "spawn a thread");
            delete target;
            return;
        }
        // It has only the reference spawn() gave it, so this frees it.
        ustl::move(thread).cancel();
        auto _ = Thread::Current::sleep_for(RUN_FOR, task::Interruptible::No);
        verdict(tally, !target->ran.load(std::memory_order_acquire),
                "cancelling a thread that was never started frees it unrun");
        settle(target, true);
    }

    auto check_deferred_start(Tally &tally) -> void {
        auto *const dropped = new (mm::GAF_KERNEL) Target{};
        {
            auto thread = spawn_unstarted(dropped);
            if (!thread) {
                verdict(tally, false, "spawn a thread");
                delete dropped;
                return;
            }
            // Handed over unstarted, and the handle goes before any resume():
            // the thread has only the handle's reference, so this frees it.
            auto handle = ustl::move(thread).defer_start();
        }
        auto _ = Thread::Current::sleep_for(RUN_FOR, task::Interruptible::No);
        verdict(tally, !dropped->ran.load(std::memory_order_acquire),
                "a deferred start whose handle goes first frees the thread unrun");
        settle(dropped, true);

        auto *const target = new (mm::GAF_KERNEL) Target{};
        target->retcode = 5;
        auto thread = spawn_unstarted(target);
        if (!thread) {
            verdict(tally, false, "spawn a thread");
            delete target;
            return;
        }
        auto handle = ustl::move(thread).defer_start();
        auto _ = Thread::Current::sleep_for(RUN_FOR, task::Interruptible::No);
        auto const ran_early = target->ran.load(std::memory_order_acquire);
        // The first resume() through the handle is the start.
        auto _ = handle->resume();
        i32 retcode = -1;
        auto const status = handle.join(&retcode, time::Deadline::infinite());
        verdict(tally, !ran_early && status == Status::Ok && retcode == 5,
                "and one resumed through its handle runs only then, and is joined");
        settle(target, status == Status::Ok);
    }

    /// Per round, one thread per CPU. The even ones are joined in spawn order
    /// once all are spawned: the early ones have exited, the late ones are
    /// still starting, so both join paths run. The odd ones are detached at
    /// once, so their handles' drops race their exits. Exits race joins and
    /// drops on every CPU.
    auto check_stress(Tally &tally, usize const rounds) -> void {
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
            task::JoinHandle threads[MAX_CPUS];
            for (usize i = 0; i < width; ++i) {
                targets[i] = new (mm::GAF_KERNEL) Target{};
                targets[i]->retcode = static_cast<i32>(round * width + i);
                threads[i] = spawn_target(targets[i], static_cast<CpuNum>(i));
                if (i % 2 == 1) {
                    threads[i].detach();
                }
            }
            for (usize i = 0; i < width; ++i) {
                if (i % 2 == 1) {
                    continue;
                }
                if (!threads[i]) {
                    bad += 1;
                    delete targets[i];
                    continue;
                }
                i32 retcode = -1;
                auto const status = threads[i].join(&retcode, time::Deadline::infinite());
                if (status != Status::Ok || retcode != targets[i]->retcode) {
                    bad += 1;
                }
                settle(targets[i], status == Status::Ok);
            }
            for (usize i = 1; i < width; i += 2) {
                auto *const target = targets[i];
                auto const returned = poll_until(
                    [target] { return target->returning.load(std::memory_order_acquire); });
                if (!returned) {
                    bad += 1;
                }
                settle(target, returned);
            }
        }
        auto const elapsed = time::SteadyClock::now() - started;
        log::info("threadjoin.test:   stress: {} round(s) x {} thread(s) in {} ms, {} bad", rounds,
                  width, duration_cast<Milliseconds>(elapsed).count(), bad);
        verdict(tally, bad == 0,
                "every stressed thread is joined with its return code or runs to its end");
    }

    auto parse_rounds(int argc, char **argv) -> usize {
        if (argc < 3 || std::string_view(argv[1]) != "stress") {
            return 0;
        }
        usize rounds = 0;
        for (auto const c: std::string_view(argv[2])) {
            if (c < '0' || c > '9') {
                return 0;
            }
            rounds = rounds * 10 + static_cast<usize>(c - '0');
        }
        return rounds;
    }

    auto threadjoin_test_command(int argc, char **argv) -> int {
        Tally tally;

        check_join_after_exit(tally);
        check_join_waits(tally);
        check_join_times_out(tally);
        check_moved_handle(tally);
        check_detach_running(tally);
        check_detach_after_exit(tally);
        check_cancel(tally);
        check_deferred_start(tally);
        if (auto const rounds = parse_rounds(argc, argv); rounds != 0) {
            check_stress(tally, rounds);
        }

        if (tally.failed != 0) {
            log::error("threadjoin.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("threadjoin.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(ThreadJoinTest, "threadjoin.test", ours::threadjoin_test_command,
             "threadjoin.test [stress ROUNDS]: a thread's JoinHandle against its contract");
