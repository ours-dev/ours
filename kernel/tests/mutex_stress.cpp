/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// MutexStress.AllWorkersFinishBeforeTheTimeout: hammer one sync::Mutex from
// three threads pinned to three different CPUs, and check that every thread
// came back.
//
// It exists for one bug class: the lost wakeup, where a contender decides to
// park, the owner releases before the contender is on the queue, and the
// contender then sleeps on a mutex nobody will ever release. Nothing in a
// normal boot contends a mutex often enough to hit that window reliably --
// it took a probe test killing processes one after another to see it at all
// -- so this manufactures the contention instead. Two of the threads hold the
// mutex across a short critical section, so the others have time to arrive
// and fail their CAS; the third takes and drops it with nothing in between,
// so releases are frequent enough to land in the gap between a contender's
// failed CAS and its enqueue.
//
// A lost wakeup shows up as a thread that never finishes, so the verdict is
// "all three finished before the timeout". Mutual exclusion is checked on
// the way: the counter is a plain integer bumped under the mutex by
// read-spin-write, so two threads inside at once lose increments, and
// holder() must name the thread that just acquired.
//
// Why an in-kernel test: a mutex cannot be tested on the host -- its slow
// path is the scheduler.
#include <ours/cpu.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>

#include <arch/spin_hint.hpp>
#include <heap/new.hpp>
#include <ktest/ktest.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    constexpr usize DEFAULT_ROUNDS = 20000;
    constexpr usize NUM_WORKERS = 3;

    /// How long the test waits for the workers before calling them stuck.
    /// Generous on purpose: under QEMU's TCG every park and wake is a pair of
    /// context switches on emulated CPUs -- the default run takes about three
    /// seconds there -- and a false "stuck" would send someone hunting a bug
    /// that is not there.
    ///
    /// Measured on the clock rather than by counting polls: a 10 ms sleep
    /// rounds up to the tick and then some, and a count of them overstated
    /// the wait by a factor of four.
    constexpr auto STUCK_AFTER = Seconds(20);
    constexpr auto POLL_INTERVAL = Milliseconds(10);

    struct StressState {
        sync::Mutex mutex;
        /// Guarded by |mutex|. Deliberately not atomic -- see the file note.
        usize counter = 0;
        usize rounds = 0;
        std::atomic<usize> finished{0};
        std::atomic<usize> violations{0};
    };

    struct Worker {
        StressState *state;
        /// Spin-hint iterations inside the critical section; zero makes the
        /// thread a pure take-and-drop churner.
        usize hold_spins;
    };

    auto worker_routine(void *arg) -> i32 {
        auto const &worker = *static_cast<Worker const *>(arg);
        auto &state = *worker.state;
        auto *const self = task::Thread::Current::get();

        for (usize i = 0; i < state.rounds; ++i) {
            Guard guard{state.mutex};
            if (state.mutex.holder() != self) {
                state.violations.fetch_add(1, std::memory_order_relaxed);
            }

            auto const before = state.counter;
            for (usize k = 0; k < worker.hold_spins; ++k) {
                arch::spin_hint();
            }
            state.counter = before + 1;
        }

        // Last thing this thread does with |state|: once the test sees
        // all three here, it frees it.
        state.finished.fetch_add(1, std::memory_order_release);
        return 0;
    }

    KTEST(MutexStress, AllWorkersFinishBeforeTheTimeout) {
        auto const num_cpus = Cpu::num_online_cpus();
        if (num_cpus < 2) {
            KTEST_SKIP("needs at least two online CPUs");
        }
        auto const rounds = DEFAULT_ROUNDS;

        // Heap, not the stack: on the failure path the stuck threads still
        // point at it after the test returns, so it has to be leaked
        // rather than popped.
        auto *const state = new (mm::ALLOC_KERNEL) StressState{};
        auto *const workers = new (mm::ALLOC_KERNEL) Worker[NUM_WORKERS]{
            {state, 64},
            {state, 64},
            {state, 0},
        };
        ASSERT_NONNULL(state, "allocate the stress state");
        ASSERT_NONNULL(workers, "allocate the worker arguments");
        state->rounds = rounds;

        auto const started = time::SteadyClock::now();
        for (usize i = 0; i < NUM_WORKERS; ++i) {
            auto *const thread = task::Thread::spawn("mutex-stress", worker_routine, &workers[i],
                                                     task::DEFAULT_PRIORITY);
            // On failure the ones already running still count down
            // |finished|; there is no way to call them back, so the state is
            // left to them.
            ASSERT_NONNULL(thread, "spawn worker {}", i);
            // One CPU each, skipping CPU 0 when there are enough to spare, so
            // the contention is real parallelism and not time-slicing.
            thread->set_cpu_affinity_hard(CpuMask::of(static_cast<CpuNum>((i + 1) % num_cpus)));
            // detach() and resume() separately: detach_and_resume() is declared
            // but has no definition yet.
            auto _ = thread->detach();
            auto _ = thread->resume();
        }

        auto const give_up = started + STUCK_AFTER;
        while (state->finished.load(std::memory_order_acquire) < NUM_WORKERS &&
               time::SteadyClock::now() < give_up) {
            auto _ = task::Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }
        auto const elapsed = time::SteadyClock::now() - started;

        auto const finished = state->finished.load(std::memory_order_acquire);
        if (finished < NUM_WORKERS) {
            // A worker parked and never woke. Leak the state: it is still
            // somebody's mutex.
            auto *const holder = state->mutex.holder();
            log::error("mutexstress: STUCK -- {}/{} workers finished after {} ms, holder {}",
                       finished, NUM_WORKERS, duration_cast<Milliseconds>(elapsed).count(),
                       static_cast<void *>(holder));
            FAIL("all three workers finish before the timeout; leaking the state");
        }

        auto const expected = rounds * NUM_WORKERS;
        auto const counter = state->counter;
        auto const violations = state->violations.load(std::memory_order_relaxed);
        delete[] workers;
        delete state;

        EXPECT_EQ(counter, expected, "no increment is lost, so no two threads were inside at once");
        EXPECT_EQ(violations, 0u, "holder() names the thread that just acquired");
        log::info("mutexstress: {} rounds x {} threads in {} us", rounds, NUM_WORKERS,
                  duration_cast<Microseconds>(elapsed).count());
    }
} // namespace
} // namespace ours
