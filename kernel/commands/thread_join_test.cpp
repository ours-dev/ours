/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `threadjoin.test [stress ROUNDS]`: Thread::join() and detach() against the
// contract on their declarations (kernel/include/ours/task/thread.hpp) and
// design option B (.agents/tasks/thread-join/DESIGN.md).
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
// `stress ROUNDS` adds, per round, one joinable thread per CPU, each joined
// at once, so exits race joins on every CPU. Run it on dbg_1, where lockdep
// and the PI-lock assertions are compiled in.
//
// It is also the failing test of the milestone's hand-written function
// (docs/authored.md): until join() is written, every check that joins fails
// with NotSupported. Each case is written so that a join() that did not reap
// its thread leaves the shared state leaked rather than freed under a thread
// that may still use it.
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
        // freed by the command as soon as it sees that, so nothing may read
        // |t| after it.
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
        log::error("threadjoin.test:   the thread was not reaped; leaking its state");
    }

    auto check_join_after_exit(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        target->retcode = 42;
        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            verdict(tally, false, "spawn a joinable thread");
            delete target;
            return;
        }
        if (!poll_until([thread] { return is_dead(thread); })) {
            verdict(tally, false, "the thread exits");
            settle(target, false);
            return;
        }

        i32 retcode = -1;
        auto const status = thread->join(&retcode, time::Deadline::infinite());
        log::info("threadjoin.test:   join after exit answered {}, retcode {}", to_string(status),
                  retcode);
        verdict(tally, status == Status::Ok && retcode == 42,
                "joining a thread that has exited returns its return code");
        settle(target, status == Status::Ok);
    }

    auto check_join_waits(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        target->retcode = 7;
        target->run_a_while = true;
        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            verdict(tally, false, "spawn a joinable thread");
            delete target;
            return;
        }

        i32 retcode = -1;
        auto const status = thread->join(&retcode, time::Deadline::infinite());
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
        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            verdict(tally, false, "spawn a joinable thread");
            delete target;
            return;
        }

        i32 retcode = -1;
        auto const started = time::SteadyClock::now();
        auto const first = thread->join(&retcode, deadline_after(SHORT_WAIT));
        auto const elapsed = time::SteadyClock::now() - started;
        verdict(tally, first == Status::TimeOut && retcode == -1 && elapsed >= SHORT_WAIT,
                "a join that reaches its deadline answers TimeOut and leaves retcode alone");

        target->release.signal();
        auto const second = thread->join(&retcode, time::Deadline::infinite());
        verdict(tally, second == Status::Ok && retcode == 3,
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
        // Last touch of |j|: once the command sees this, it frees it.
        j.done.store(true, std::memory_order_release);
        return 0;
    }

    auto check_one_reaper(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        auto *const joiner = new (mm::GAF_KERNEL) Joiner{};
        target->retcode = 4;
        target->wait_for_release = true;

        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            verdict(tally, false, "spawn a joinable thread");
            delete target;
            delete joiner;
            return;
        }
        joiner->target_thread = thread;
        auto *const joiner_thread =
            Thread::spawn("join-test-joiner", joiner_routine, joiner, task::DEFAULT_PRIORITY);
        if (joiner_thread == nullptr) {
            verdict(tally, false, "spawn the joiner");
            target->release.signal();
            settle(target, false);
            delete joiner;
            return;
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
        verdict(tally, claimed, "the joiner waits in join()");

        if (claimed) {
            i32 retcode = -1;
            auto const second = thread->join(&retcode, deadline_after(SHORT_WAIT));
            verdict(tally, second == Status::BadState,
                    "a second join while one is in progress answers BadState");
            verdict(tally, thread->detach() == Status::BadState,
                    "and so does detach(): the joiner holds the claim");
        }

        target->release.signal();
        auto const finished =
            poll_until([joiner] { return joiner->done.load(std::memory_order_acquire); });
        auto const reaped = finished && joiner->status == Status::Ok;
        verdict(tally, reaped && joiner->retcode == 4, "the joiner reaps the thread");
        settle(target, reaped);
        if (finished) {
            delete joiner;
        }
    }

    auto check_join_of_detached(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        target->wait_for_release = true;
        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            verdict(tally, false, "spawn a joinable thread");
            delete target;
            return;
        }
        auto const detached = thread->detach();
        verdict(tally, detached == Status::Ok, "a running thread can be detached");

        i32 retcode = -1;
        auto const status = thread->join(&retcode, deadline_after(SHORT_WAIT));
        verdict(tally, status == Status::BadState, "joining a detached thread answers BadState");

        // Detached, it frees itself once released, and stops reading
        // |target| once it says it is returning.
        target->release.signal();
        auto const returned =
            poll_until([target] { return target->returning.load(std::memory_order_acquire); });
        settle(target, returned);
    }

    auto check_detach_after_exit(Tally &tally) -> void {
        auto *const target = new (mm::GAF_KERNEL) Target{};
        auto *const thread = spawn_joinable(target);
        if (thread == nullptr) {
            verdict(tally, false, "spawn a joinable thread");
            delete target;
            return;
        }
        if (!poll_until([thread] { return is_dead(thread); })) {
            verdict(tally, false, "the thread exits");
            settle(target, false);
            return;
        }
        auto const status = thread->detach();
        verdict(tally, status == Status::Ok, "detach() of a thread that has exited reaps it");
        settle(target, status == Status::Ok);
    }

    /// Per round, one joinable thread per CPU, joined in spawn order as soon
    /// as all are spawned: the early ones have exited, the late ones are still
    /// starting, so both join paths run, and exits race joins on every CPU.
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
            Thread *threads[MAX_CPUS] = {};
            for (usize i = 0; i < width; ++i) {
                targets[i] = new (mm::GAF_KERNEL) Target{};
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
        log::info("threadjoin.test:   stress: {} round(s) x {} thread(s) in {} ms, {} bad", rounds,
                  width, duration_cast<Milliseconds>(elapsed).count(), bad);
        verdict(tally, bad == 0, "every stressed join reaps its thread with its return code");
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
        check_one_reaper(tally);
        check_join_of_detached(tally);
        check_detach_after_exit(tally);
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
             "threadjoin.test [stress ROUNDS]: join and detach against their contract");
