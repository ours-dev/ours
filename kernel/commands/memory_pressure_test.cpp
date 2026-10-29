/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `mempressure.test`: the PMM's gate in front of the allocations that can
// wait, and the free-memory signal for whoever watches memory
// (.agents/tasks/memory-pressure, step 1).
//
//   - with no delay level, a CanWait allocation is served;
//   - with the level above free memory, a CanWait allocation shuts the gate
//     and answers ShouldWait, while one without CanWait is still served;
//   - a waiter sleeps while the gate is shut, and wakes with a frame when it
//     opens;
//   - with delayed allocations refused, a waiter wakes with OutOfMem, and a
//     CanWait allocation below the level fails at once;
//   - an allocation that takes free memory below the lower bound signals the
//     watcher's event, and a free that takes it above the upper bound does.
//
// It leaves the gate as boot left it: open, with no level and no event.
//
// Why a console command: the gate counts the real zones' free frames, and a
// waiter is a real thread asleep on the real event.
#include <ours/mm/gaf.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/sync/event.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>

#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using task::Thread;

    constexpr auto WAITABLE = mm::GAF_USER | mm::Gaf::CanWait;
    constexpr auto NO_BOUND = ~usize{0};

    /// How long a waiter is given to fall asleep, and an event to be
    /// signalled, before the check looks.
    constexpr auto SETTLE = Milliseconds(200);
    /// How long one wait of a waiter lasts: a waiter the gate never wakes
    /// gives up after it rather than holding the console.
    constexpr auto LOST_AFTER = Seconds(5);

    /// Frames taken, or given back, to move free memory across a bound,
    /// against a bound this many frames away.
    constexpr usize NR_CROSSING = 64;
    constexpr usize MARGIN = NR_CROSSING / 2;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("mempressure.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("mempressure.test: FAIL  {}", name);
    }

    auto deadline_after(Duration const delta) -> time::Deadline {
        return time::Deadline::at(time::SteadyClock::now() + delta);
    }

    /// The gate as boot leaves it.
    auto open_gate() -> void {
        auto _ = mm::set_free_memory_signal(0, NO_BOUND, 0, nullptr);
    }

    /// A gate shut by a CanWait allocation, with a level no free count can
    /// reach. Whether it answered ShouldWait.
    auto shut_gate() -> bool {
        auto _ = mm::set_free_memory_signal(0, NO_BOUND, NO_BOUND, nullptr);
        auto const delayed = mm::try_alloc_frames(WAITABLE, 0);
        if (delayed) {
            mm::free_frames(*delayed, 0);
            return false;
        }
        return Status::ShouldWait == delayed.unwrap_err();
    }

    /// What one waiter came back with.
    struct Waiter {
        Status status = Status::BadState;
        mm::PmFrame *frame = nullptr;
    };

    /// Waits for a frame as the anonymous page request will: a ShouldWait is
    /// a race lost to a gate shut again, and the wait starts over.
    auto wait_routine(void *const arg) -> i32 {
        auto &waiter = *static_cast<Waiter *>(arg);
        while (true) {
            auto result =
                mm::wait_for_frame_allocation(WAITABLE, deadline_after(LOST_AFTER), false);
            if (result) {
                waiter.frame = *result;
                waiter.status = Status::Ok;
                return 0;
            }
            if (Status::ShouldWait != result.unwrap_err()) {
                waiter.status = result.unwrap_err();
                return 0;
            }
        }
    }

    auto start(Waiter *const waiter) -> Thread * {
        auto *const thread =
            Thread::spawn("mempressure-waiter", wait_routine, waiter, task::DEFAULT_PRIORITY);
        if (thread != nullptr) {
            auto _ = thread->resume();
        }
        return thread;
    }

    /// Whether |thread| is still running after SETTLE: asleep in its wait.
    auto still_waiting(Thread *const thread) -> bool {
        i32 retcode = 0;
        return thread->join(&retcode, deadline_after(SETTLE)) == Status::TimeOut;
    }

    /// Joins |thread|, which ends by itself: each of its waits gives up after
    /// LOST_AFTER. Whether it had ended within one of them.
    auto finish(Thread *const thread) -> bool {
        i32 retcode = 0;
        if (thread->join(&retcode, deadline_after(LOST_AFTER)) == Status::Ok) {
            return true;
        }
        auto _ = thread->join(&retcode, time::Deadline::infinite());
        return false;
    }

    auto check_gate(Tally &tally) -> void {
        open_gate();
        auto served = mm::try_alloc_frames(WAITABLE, 0);
        verdict(tally, !!served, "with no delay level, a CanWait allocation is served");
        if (served) {
            mm::free_frames(*served, 0);
        }

        auto const shut = shut_gate();
        auto plain = mm::try_alloc_frames(mm::GAF_USER, 0);
        verdict(tally, shut && !!plain,
                "below the delay level a CanWait allocation answers ShouldWait, and one without "
                "CanWait is served");
        if (plain) {
            mm::free_frames(*plain, 0);
        }
    }

    auto check_waiter(Tally &tally) -> void {
        auto const shut = shut_gate();
        Waiter waiter;
        auto *const thread = start(&waiter);
        if (thread == nullptr) {
            verdict(tally, false, "a waiter thread is started");
            open_gate();
            return;
        }
        auto const asleep = still_waiting(thread);
        open_gate();
        auto const ended = finish(thread);
        verdict(tally,
                shut && asleep && ended && waiter.status == Status::Ok && waiter.frame != nullptr,
                "a waiter sleeps while the gate is shut, and wakes with a frame when it opens");
        if (waiter.frame != nullptr) {
            mm::free_frames(waiter.frame, 0);
        }
    }

    auto check_refusal(Tally &tally) -> void {
        auto const shut = shut_gate();
        Waiter waiter;
        auto *const thread = start(&waiter);
        if (thread == nullptr) {
            verdict(tally, false, "a waiter thread is started");
            open_gate();
            return;
        }
        auto const asleep = still_waiting(thread);
        mm::refuse_delayed_allocations();
        auto const ended = finish(thread);
        auto refused = mm::try_alloc_frames(WAITABLE, 0);
        verdict(tally,
                shut && asleep && ended && waiter.status == Status::OutOfMem &&
                    waiter.frame == nullptr && !refused && refused.unwrap_err() == Status::OutOfMem,
                "with delayed allocations refused, a waiter wakes with OutOfMem, and a CanWait "
                "allocation below the level fails at once");
        if (waiter.frame != nullptr) {
            mm::free_frames(waiter.frame, 0);
        }
        if (refused) {
            mm::free_frames(*refused, 0);
        }
        open_gate();
    }

    auto check_signal(Tally &tally) -> void {
        sync::Event watched;
        std::array<mm::PmFrame *, NR_CROSSING> frames{};

        // Free memory pushed below the lower bound by allocations.
        auto const before = mm::approx_free_frame_count();
        auto const armed_low = mm::set_free_memory_signal(before - MARGIN, NO_BOUND, 0, &watched);
        usize taken = 0;
        for (auto &frame: frames) {
            auto result = mm::try_alloc_frames(mm::GAF_USER, 0);
            if (!result) {
                break;
            }
            frame = *result;
            taken += 1;
        }
        auto const low = watched.wait(deadline_after(SETTLE)) == Status::Ok;
        verdict(tally, armed_low && taken == NR_CROSSING && low,
                "an allocation that takes free memory below the lower bound signals the event");
        watched.unsignal();

        // And above the upper bound by frees.
        auto const during = mm::approx_free_frame_count();
        auto const armed_high = mm::set_free_memory_signal(0, during + MARGIN, 0, &watched);
        for (usize i = 0; i < taken; ++i) {
            mm::free_frames(frames[i], 0);
        }
        auto const high = watched.wait(deadline_after(SETTLE)) == Status::Ok;
        verdict(tally, armed_high && high,
                "a free that takes free memory above the upper bound signals the event");
        open_gate();
    }

    auto mempressure_test_command(int, char **) -> int {
        Tally tally;
        check_gate(tally);
        check_waiter(tally);
        check_refusal(tally);
        check_signal(tally);
        open_gate();

        if (tally.failed != 0) {
            log::error("mempressure.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("mempressure.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(MemoryPressureTest, "mempressure.test", ours::mempressure_test_command,
             "check the PMM's gate for allocations that can wait, and its free-memory signal");
