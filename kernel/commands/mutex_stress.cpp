/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `mutex.stress [rounds]`: hammer one sync::Mutex from three threads pinned
// to three different CPUs, and report whether every thread came back.
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
// This is a console command rather than a unit test because the tree has no
// in-kernel unittest runtime yet (see //kernel/products/bin:vmours.with-tests),
// and a mutex cannot be tested on the host: its slow path is the scheduler.
#include <ours/cpu.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>
#include <string_view>

#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    constexpr usize DEFAULT_ROUNDS = 20000;
    constexpr usize NUM_WORKERS = 3;

    /// How long the command waits for the workers before calling them stuck.
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

        // Last thing this thread does with |state|: once the command sees
        // all three here, it frees it.
        state.finished.fetch_add(1, std::memory_order_release);
        return 0;
    }

    auto parse_rounds(int argc, char **argv) -> usize {
        if (argc < 2) {
            return DEFAULT_ROUNDS;
        }

        usize rounds = 0;
        for (auto const c: std::string_view(argv[1])) {
            if (c < '0' || c > '9') {
                return 0;
            }
            rounds = rounds * 10 + static_cast<usize>(c - '0');
        }
        return rounds;
    }

    auto mutex_stress_command(int argc, char **argv) -> int {
        auto const rounds = parse_rounds(argc, argv);
        if (rounds == 0) {
            log::warn("mutex.stress: rounds must be a positive decimal number");
            return 1;
        }

        auto const num_cpus = Cpu::num_online_cpus();
        if (num_cpus < 2) {
            log::warn("mutex.stress: needs at least two online CPUs, have {}", num_cpus);
            return 1;
        }

        // Heap, not the stack: on the failure path the stuck threads still
        // point at it after this command returns, so it has to be leaked
        // rather than popped.
        auto *const state = new (mm::GAF_KERNEL) StressState{};
        auto *const workers = new (mm::GAF_KERNEL) Worker[NUM_WORKERS]{
            {state, 64},
            {state, 64},
            {state, 0},
        };
        if (state == nullptr || workers == nullptr) {
            log::warn("mutex.stress: out of memory");
            return 1;
        }
        state->rounds = rounds;

        auto const started = time::SteadyClock::now();
        for (usize i = 0; i < NUM_WORKERS; ++i) {
            auto *const thread = task::Thread::spawn("mutex-stress", worker_routine, &workers[i],
                                                     task::DEFAULT_PRIORITY);
            if (thread == nullptr) {
                // The ones already running still count down |finished|; there
                // is no way to call them back, so leave the state to them.
                log::warn("mutex.stress: could not spawn worker {}", i);
                return 1;
            }
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
            log::error("mutex.stress: STUCK -- {}/{} workers finished after {} ms, holder {}",
                       finished, NUM_WORKERS, duration_cast<Milliseconds>(elapsed).count(),
                       static_cast<void *>(holder));
            return 1;
        }

        auto const expected = rounds * NUM_WORKERS;
        auto const counter = state->counter;
        auto const violations = state->violations.load(std::memory_order_relaxed);
        delete[] workers;
        delete state;

        if (counter != expected || violations != 0) {
            log::error("mutex.stress: FAILED -- counter {} (expected {}), {} holder mismatches",
                       counter, expected, violations);
            return 1;
        }

        log::info("mutex.stress: ok -- {} rounds x {} threads in {} us", rounds, NUM_WORKERS,
                  duration_cast<Microseconds>(elapsed).count());
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(MutexStress, "mutex.stress", ours::mutex_stress_command,
             "mutex.stress [rounds]: contend one sync::Mutex from three CPUs")
