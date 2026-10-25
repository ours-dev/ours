/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `preempt.test`: a slice ends at its CPU's preemption deadline, a slot in the
// CPU's timer queue, and nothing else ends it (step 3 of
// .agents/tasks/idle-power/PLAN.md). What it checks, over one second on a
// quiet machine:
//
//   - two threads spinning on the highest online CPU take turns, and every
//     turn ends within a little of the slice: none runs on, which no tick
//     would stop now, and most are a whole slice long;
//   - a timer due on that CPU before the running slice ends fires on time:
//     the device is armed for the earlier of the two;
//   - no other CPU passes a preemption deadline: an idle thread has no slice,
//     so an idle CPU is woken by its timers alone;
//   - two threads handing a token back and forth on that CPU set its
//     deadline once per switch: a switch consumes the request for it, and no
//     second reschedule follows to pick the same thread again.
//
// A turn is seen from inside the spinner: a jump of more than GAP between two
// readings of the clock is a time it did not run, and the stretch between two
// jumps is one turn.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/ipi_service.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/sync/semaphore.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/cfg.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/timer.hpp>
#include <ours/time/timer_queue.hpp>

#include <atomic>

#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using task::Thread;

    constexpr auto WINDOW = Milliseconds(1000);
    constexpr auto LOST_AFTER = Seconds(5);
    /// A jump in the clock longer than this, seen by a spinner, is a time it
    /// did not run. Far above an interrupt's cost, far below a slice.
    constexpr auto GAP = Milliseconds(1);
    /// How much later than the slice a turn may end with a one-shot device:
    /// the interrupt's latency and the switch. Under TCG a timer alone fires
    /// up to about 1.3 ms late (timer.test), and a turn ends about 1.5 ms past
    /// its slice.
    constexpr auto LATE_ONESHOT = Milliseconds(4);
    /// How much shorter than the slice a turn may be and still count as a
    /// whole one.
    constexpr auto EARLY = Milliseconds(1);
    /// How far ahead the probe timer is due: well inside a slice. How late it
    /// may fire with a one-shot device: timer.test's bound, a fifth of a tick.
    /// A device armed for the slice's end instead would make it late by the
    /// rest of the slice, tens of milliseconds.
    constexpr auto PROBE_IN = Milliseconds(5);
    constexpr auto PROBE_LATE = Milliseconds(2);
    constexpr usize MAX_TURNS = 64;
    constexpr usize NUM_SPINNERS = 2;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("preempt.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("preempt.test: FAIL  {}", name);
    }

    /// One spinner's turns, in the order it had them. Written by the spinner
    /// only, read once it is joined.
    struct Spinner {
        Duration turns[MAX_TURNS];
        usize num_turns = 0;
    };

    /// The test's state, on the heap and freed only once every thread and
    /// the timer that read it are done with it.
    struct State {
        std::atomic<bool> stop{false};
        Spinner spinners[NUM_SPINNERS];

        time::Timer probe;
        TimePoint<> probe_due;
        std::atomic<bool> probe_fired{false};
        TimePoint<> probe_fired_at;
    };

    struct SpinnerArg {
        State *state;
        Spinner *spinner;
    };
    SpinnerArg SPINNER_ARGS[NUM_SPINNERS];

    /// Records a turn each time it comes back after a jump: the one that
    /// ended at the last reading before the jump.
    auto spinner_routine(void *const arg) -> i32 {
        auto &[state, spinner] = *static_cast<SpinnerArg *>(arg);
        auto turn_began = time::SteadyClock::now();
        auto last = turn_began;
        while (!state->stop.load(std::memory_order_acquire)) {
            auto const now = time::SteadyClock::now();
            if (now - last > GAP) {
                if (spinner->num_turns < MAX_TURNS) {
                    spinner->turns[spinner->num_turns] = last - turn_began;
                    spinner->num_turns += 1;
                }
                turn_began = now;
            }
            last = now;
        }
        return 0;
    }

    auto probe_expired(State *const state, time::Timer *) -> time::Timer::HandlerResult {
        state->probe_fired_at = time::SteadyClock::now();
        state->probe_fired.store(true, std::memory_order_release);
        return time::Timer::HandlerResult::Handled;
    }

    /// On the spinners' CPU, from its IPI: arms the probe on that CPU's queue.
    auto arm_probe(void *const context) -> void {
        auto *const state = static_cast<State *>(context);
        state->probe_due = time::SteadyClock::now() + PROBE_IN;
        state->probe.activate(time::Deadline::at(state->probe_due), probe_expired, state);
    }

    auto spawn_pinned(SpinnerArg *const arg, CpuNum const cpu) -> Thread * {
        auto *const thread =
            Thread::spawn("preempt.test/spin", spinner_routine, arg, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        auto _ = thread->resume();
        return thread;
    }

    /// The hand-off: |ping| takes the token to the second thread, |pong| back.
    /// The first counts the CPU's deadline sets at either end.
    constexpr usize HAND_OFFS = 200;
    struct HandOff {
        CpuNum cpu = INVALID_CPU_NUM;
        sync::Semaphore ping;
        sync::Semaphore pong;
        u64 resets = 0;
    };
    HandOff HAND_OFF;

    auto hand_off_first(void *const arg) -> i32 {
        auto &hand_off = *static_cast<HandOff *>(arg);
        auto const before = time::TimerQueue::num_preempt_resets(hand_off.cpu);
        for (usize i = 0; i < HAND_OFFS; ++i) {
            hand_off.ping.post();
            auto _ = hand_off.pong.wait();
        }
        hand_off.resets = time::TimerQueue::num_preempt_resets(hand_off.cpu) - before;
        return 0;
    }

    auto hand_off_second(void *const arg) -> i32 {
        auto &hand_off = *static_cast<HandOff *>(arg);
        for (usize i = 0; i < HAND_OFFS; ++i) {
            auto _ = hand_off.ping.wait();
            hand_off.pong.post();
        }
        return 0;
    }

    /// Two switches a round trip, each one pick and so one deadline set; a
    /// tenth more for whatever else the CPU picks meanwhile. A request left
    /// set by a switch would double it.
    auto check_hand_off(Tally &tally, CpuNum const cpu) -> void {
        HAND_OFF.cpu = cpu;
        Thread *threads[2]{};
        Thread::StartRoutine const routines[2] = {hand_off_second, hand_off_first};
        for (usize i = 0; i < 2; ++i) {
            threads[i] =
                Thread::spawn("preempt.test/hand", routines[i], &HAND_OFF, task::DEFAULT_PRIORITY);
            if (threads[i] != nullptr) {
                threads[i]->set_cpu_affinity_hard(CpuMask::of(cpu));
                auto _ = threads[i]->resume();
            }
        }
        auto joined = true;
        for (auto *const thread: threads) {
            joined &=
                thread != nullptr &&
                thread->join(nullptr, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
                    Status::Ok;
        }
        auto const switches = 2 * HAND_OFFS;
        log::info("preempt.test: a hand-off of {} switches set the deadline {} times", switches,
                  HAND_OFF.resets);
        verdict(tally, joined && HAND_OFF.resets <= switches + switches / 10,
                "a hand-off sets the deadline once per switch, not twice");
    }

    auto preempt_test_command(int, char **) -> int {
        Tally tally;
        auto const online = Cpu::online_mask();
        if (online.count() < 2) {
            log::info("preempt.test: ok -- 0 checks, one CPU only");
            return 0;
        }
        auto target = INVALID_CPU_NUM;
        online.for_each([&target](CpuNum const cpu) { target = cpu; });

        auto const slice = Nanoseconds(task::sched::scheduling_period().raw());
        // A periodic tick finds a passed deadline at the next tick, not at it.
        auto const late = time::TickDevice::Current::is_mode_periodic()
                              ? LATE_ONESHOT + time::NSEC_PER_TICK
                              : Duration(LATE_ONESHOT);
        auto const probe_late = time::TickDevice::Current::is_mode_periodic()
                                    ? PROBE_LATE + time::NSEC_PER_TICK
                                    : Duration(PROBE_LATE);
        log::info("preempt.test: CPU {}, slice {} us, late by at most {} us", target,
                  slice.count() / 1000, late.count() / 1000);

        auto *const state = new (mm::GAF_KERNEL) State();
        if (state == nullptr) {
            log::error("preempt.test: FAILED -- no memory for the test's state");
            return 1;
        }

        u64 preempts_before[MAX_CPU];
        online.for_each([&preempts_before](CpuNum const cpu) {
            preempts_before[cpu] = time::TimerQueue::num_preempts(cpu);
        });

        Thread *threads[NUM_SPINNERS]{};
        auto spawned = true;
        for (usize i = 0; i < NUM_SPINNERS; ++i) {
            SPINNER_ARGS[i] = SpinnerArg{state, &state->spinners[i]};
            threads[i] = spawn_pinned(&SPINNER_ARGS[i], target);
            spawned &= threads[i] != nullptr;
        }
        verdict(tally, spawned, "two spinners start on the CPU");

        // Halfway through, so that a slice is running on the CPU.
        auto _ = Thread::Current::sleep_for(WINDOW / 2, task::Interruptible::No);
        IpiService::sync_exec(CpuMask::of(target), arm_probe, state);
        auto _ = Thread::Current::sleep_for(WINDOW / 2, task::Interruptible::No);

        state->stop.store(true, std::memory_order_release);
        auto joined = true;
        for (auto *const thread: threads) {
            joined &=
                thread != nullptr &&
                thread->join(nullptr, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
                    Status::Ok;
        }
        verdict(tally, joined, "the spinners are joined");

        // Each spinner's first turn is cut short by the other's arrival, a
        // wakeup on its CPU, so only the later ones count.
        auto num_turns = usize{0};
        auto num_whole = usize{0};
        auto longest = Duration::zero();
        for (auto const &spinner: state->spinners) {
            for (usize i = 1; i < spinner.num_turns; ++i) {
                auto const turn = spinner.turns[i];
                num_turns += 1;
                num_whole += turn >= slice - EARLY && turn <= slice + late ? 1 : 0;
                longest = turn > longest ? turn : longest;
            }
        }
        log::info("preempt.test: {} turns, {} of them a whole slice, the longest {} us", num_turns,
                  num_whole, longest.count() / 1000);
        verdict(tally, num_turns >= 4, "the spinners take turns");
        verdict(tally, longest <= slice + late, "no turn runs past its slice");
        verdict(tally, num_whole * 2 >= num_turns, "most turns are a whole slice");

        auto const fired = state->probe_fired.load(std::memory_order_acquire);
        auto const probe_lateness = fired ? state->probe_fired_at - state->probe_due : Duration{};
        if (fired) {
            log::info("preempt.test: the probe timer fired {} us late",
                      probe_lateness.count() / 1000);
        }
        verdict(tally, fired && probe_lateness <= probe_late,
                "a timer due before the slice ends fires on time");
        auto _ = state->probe.cancel();

        auto others_quiet = true;
        online.for_each([&](CpuNum const cpu) {
            if (cpu == target) {
                return;
            }
            auto const passed = time::TimerQueue::num_preempts(cpu) - preempts_before[cpu];
            if (passed != 0) {
                log::info("preempt.test: CPU {} passed {} preemption deadlines", cpu, passed);
                others_quiet = false;
            }
        });
        verdict(tally, others_quiet, "no other CPU passes a preemption deadline");

        check_hand_off(tally, target);

        if (tally.failed != 0) {
            log::error("preempt.test: FAILED -- {} of {} checks; the test's state is leaked",
                       tally.failed, tally.passed + tally.failed);
            return 1;
        }
        delete state;
        log::info("preempt.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(PreemptTest, "preempt.test", ours::preempt_test_command,
             "check that a slice ends at its CPU's preemption deadline, and only there");
