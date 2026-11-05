/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Dpc.StopsAndStartsWorkers: a CPU's Dpc workers stopped and started again
// (DpcRunner::stop_workers(), step 1 of .agents/tasks/cpu-hotplug/PLAN.md),
// on the highest online CPU, which taking a CPU offline will do first. What
// it checks, in order:
//
//   - a Dpc enqueued on the CPU runs there, before anything is stopped;
//   - the workers stop, and a second stop finds none (BadState);
//   - a Dpc enqueued while they are stopped stays queued and does not run;
//   - a thread that exits on the CPU meanwhile cannot be joined yet: its
//     exit queues the Dpc that tells its reaper it is off its stack, on the
//     same CPU's Normal queue, so a join times out;
//   - once the workers are started again, the queued Dpc runs there, once,
//     and the join completes with the thread's return code;
//   - with the workers stopped again, another CPU takes over a Dpc queued
//     there (DpcRunner::take_over_from(), what taking the CPU offline does
//     once it has stopped), and runs it.
//
// Why an in-kernel test: the workers are threads, and what is checked is
// where and whether the scheduler runs them.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/dpc.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>

#include <heap/new.hpp>
#include <ktest/ktest.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using task::Thread;

    /// How long a Dpc or a thread may take before the test calls it lost:
    /// under QEMU's TCG every wake is a pair of context switches on emulated
    /// CPUs.
    constexpr auto LOST_AFTER = Seconds(5);
    constexpr auto POLL_INTERVAL = Milliseconds(1);
    /// How long the test waits to see that something did not happen.
    constexpr auto SETTLE = Milliseconds(20);
    /// What the thread that exits while the workers are stopped returns.
    constexpr i32 RETCODE = 7;

    auto deadline_after(Duration const delta) -> time::Deadline {
        return time::Deadline::at(time::SteadyClock::now() + delta);
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

    /// What one probe Dpc saw: how many times it ran, and on which CPU.
    struct Probe {
        std::atomic<u32> runs{0};
        std::atomic<CpuNum> ran_on{INVALID_CPU_NUM};
    };

    auto probe_routine(Dpc *const dpc) -> void {
        auto &probe = *dpc->arg<Probe>();
        probe.ran_on.store(Cpu::id(), std::memory_order_relaxed);
        probe.runs.fetch_add(1, std::memory_order_release);
    }

    /// The test's state, on the heap: a Dpc must not be destroyed while it
    /// is queued, so a run that fails with one still queued leaks this
    /// rather than freeing it.
    struct State {
        Probe before;
        Dpc before_dpc{probe_routine, &before};
        Probe stopped;
        Dpc stopped_dpc{probe_routine, &stopped};
        Probe moved;
        Dpc moved_dpc{probe_routine, &moved};
        /// The CPU a take-over runs on, and the CPU taken over.
        CpuNum taker = INVALID_CPU_NUM;
        CpuNum taken = INVALID_CPU_NUM;
        /// Set by an enqueuer once its Dpc is queued; nothing touches the
        /// state through it after.
        std::atomic<bool> enqueued{false};
        Dpc *to_enqueue = nullptr;
    };

    auto enqueue_routine(void *const arg) -> i32 {
        auto &state = *static_cast<State *>(arg);
        auto _ = DpcRunner::enqueue(*state.to_enqueue, DpcRunner::Level::Normal);
        state.enqueued.store(true, std::memory_order_release);
        return 0;
    }

    auto exit_routine(void *) -> i32 {
        return RETCODE;
    }

    auto take_over_routine(void *const arg) -> i32 {
        auto &state = *static_cast<State *>(arg);
        DpcRunner::take_over_from(state.taken);
        return 0;
    }

    /// A thread pinned to |cpu| running |routine| on |arg|, started; null
    /// when it could not be made.
    auto spawn_on(CpuNum const cpu, char const *const name, Thread::StartRoutine const routine,
                  void *const arg, bool const detached) -> Thread * {
        auto *const thread = Thread::spawn(name, routine, arg, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        // detach() fails only for a thread that is Dead or claimed by a
        // join(), and one just spawned is neither; resume() answers only Ok.
        if (detached) {
            auto _ = thread->detach();
        }
        auto _ = thread->resume();
        return thread;
    }

    /// Enqueues |dpc| on |cpu|, from a detached thread pinned there, and
    /// waits until it is queued.
    auto enqueue_on(State &state, CpuNum const cpu, Dpc &dpc) -> bool {
        state.to_enqueue = &dpc;
        state.enqueued.store(false, std::memory_order_relaxed);
        if (spawn_on(cpu, "dpc.test/enqueue", enqueue_routine, &state, true) == nullptr) {
            return false;
        }
        return poll_until([&state] { return state.enqueued.load(std::memory_order_acquire); });
    }

    KTEST(Dpc, StopsAndStartsWorkers) {
        auto target = CpuNum{0};
        Cpu::online_mask().for_each([&target](CpuNum const cpu) { target = cpu; });
        log::info("Dpc: stopping the Dpc workers of CPU {}", target);

        auto *const state = new (mm::ALLOC_KERNEL) State();
        ASSERT_NONNULL(state, "allocate the test's state");

        auto ok = true;
        auto const first_ran = enqueue_on(*state, target, state->before_dpc) && poll_until([state] {
                                   return state->before.runs.load(std::memory_order_acquire) == 1;
                               }) &&
                               state->before.ran_on.load(std::memory_order_relaxed) == target;
        EXPECT_TRUE(first_ran, "a Dpc enqueued on the CPU runs there");
        ok = ok && first_ran;

        // If the stop fails, the CPU's Dpc workers may still hold a Dpc from
        // |state|, so this failure path leaks the state rather than freeing it
        // under them.
        ASSERT_OK(DpcRunner::stop_workers(target, deadline_after(LOST_AFTER)), "its workers stop");

        auto const second_stopped = DpcRunner::stop_workers(target, deadline_after(LOST_AFTER));
        EXPECT_EQ(second_stopped, Status::BadState, "a second stop finds no workers");
        ok = ok && second_stopped == Status::BadState;

        auto const queued = enqueue_on(*state, target, state->stopped_dpc);
        EXPECT_TRUE(queued, "a Dpc is enqueued on it while they are stopped");
        ok = ok && queued;
        auto _ = Thread::Current::sleep_for(SETTLE, task::Interruptible::No);
        auto const stayed_queued = state->stopped.runs.load(std::memory_order_acquire) == 0 &&
                                   state->stopped_dpc.state() == Dpc::State::Queued;
        EXPECT_TRUE(stayed_queued, "and stays queued, not run");
        ok = ok && stayed_queued;

        auto *const exiting = spawn_on(target, "dpc.test/exit", exit_routine, nullptr, false);
        auto retcode = i32{0};
        auto const cannot_join = exiting != nullptr &&
                                 exiting->join(&retcode, deadline_after(SETTLE)) == Status::TimeOut;
        EXPECT_TRUE(cannot_join, "a thread that exits there cannot be joined yet");
        ok = ok && cannot_join;

        DpcRunner::start_workers(target);
        auto const queued_ran = poll_until([state] {
                                    return state->stopped.runs.load(std::memory_order_acquire) != 0;
                                }) &&
                                state->stopped.ran_on.load(std::memory_order_relaxed) == target;
        EXPECT_TRUE(queued_ran, "once they start again, the queued Dpc runs there");
        ok = ok && queued_ran;
        auto _ = Thread::Current::sleep_for(SETTLE, task::Interruptible::No);
        auto const ran_once = state->stopped.runs.load(std::memory_order_acquire) == 1;
        EXPECT_TRUE(ran_once, "once");
        ok = ok && ran_once;
        auto const joined = exiting != nullptr &&
                            exiting->join(&retcode, deadline_after(LOST_AFTER)) == Status::Ok &&
                            retcode == RETCODE;
        EXPECT_TRUE(joined, "and the exited thread is joined with its return code");
        ok = ok && joined;

        // Another CPU to take over from this one, if there is one.
        Cpu::online_mask().for_each([state, target](CpuNum const cpu) {
            if (cpu != target && state->taker == INVALID_CPU_NUM) {
                state->taker = cpu;
            }
        });
        if (state->taker != INVALID_CPU_NUM) {
            state->taken = target;
            auto const moved_queued =
                DpcRunner::stop_workers(target, deadline_after(LOST_AFTER)) == Status::Ok &&
                enqueue_on(*state, target, state->moved_dpc);
            EXPECT_TRUE(moved_queued, "with the workers stopped again, a Dpc is queued there");
            ok = ok && moved_queued;
            auto *const taker =
                spawn_on(state->taker, "dpc.test/take-over", take_over_routine, state, false);
            auto const taken = taker != nullptr &&
                               taker->join(nullptr, deadline_after(LOST_AFTER)) == Status::Ok &&
                               poll_until([state] {
                                   return state->moved.runs.load(std::memory_order_acquire) == 1;
                               }) &&
                               state->moved.ran_on.load(std::memory_order_relaxed) == state->taker;
            EXPECT_TRUE(taken, "and another CPU takes it over and runs it");
            ok = ok && taken;
            DpcRunner::start_workers(target);
        }

        if (!ok) {
            // A Dpc may still be queued on |state|, so the test leaks it rather
            // than freeing it under the worker that runs it.
            FAIL("a check failed; leaking the test's state");
        }
        delete state;
    }

} // namespace
} // namespace ours
