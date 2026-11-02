/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `mempressure.test`: the PMM's gate in front of the allocations that can
// wait, the free-memory signal for whoever watches memory, and the VMOs that
// wait at the gate, and the memory watchdog (.agents/tasks/memory-pressure).
//
//   - with no delay level, a CanWait allocation is served;
//   - with the level above free memory, a CanWait allocation shuts the gate
//     and answers ShouldWait, while one without CanWait is still served;
//   - a waiter sleeps while the gate is shut, and wakes with a frame when it
//     opens;
//   - with delayed allocations refused, a waiter wakes with OutOfMem, and a
//     CanWait allocation below the level fails at once;
//   - an allocation that takes free memory below the lower bound signals the
//     watcher's event, and a free that takes it above the upper bound does;
//   - a store to a user VMO's mapping, a write of the VMO and a commit of it
//     sleep while the gate is shut, each in its own thread, and finish once
//     it opens, with the bytes where they were put;
//   - the watchdog finds Normal pressure with the boot levels; levels moved to
//     where free memory is make it Warning, allocations past the Critical
//     level make it Critical and frees bring it back; at OutOfMemory an
//     allocation that could wait is refused and the kernel's own is served;
//     and with the boot levels back it is Normal again.
//
// The checks of the gate drive it by hand, with the watchdog held
// (MemoryWatchdog::Hold), which arms it for itself again afterwards; the
// checks of the watchdog end with the boot levels back.
//
// Why a console command: the gate counts the real zones' free frames, and a
// waiter is a real thread asleep on the real event.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/memory_reclaimer.hpp>
#include <ours/mm/memory_watchdog.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/sync/event.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    using task::Thread;

    constexpr auto WAITABLE = mm::GAF_USER_VMO;
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
        auto _ = mm::set_free_memory_signal(0, NO_BOUND, 0, nullptr, mm::DelayedAllocations::Wait);
    }

    /// Delayed allocations refused, with a level no free count can reach.
    auto refuse() -> void {
        auto _ = mm::set_free_memory_signal(0, NO_BOUND, NO_BOUND, nullptr,
                                            mm::DelayedAllocations::Refuse);
    }

    /// A gate shut by a CanWait allocation, with a level no free count can
    /// reach. Whether it answered ShouldWait.
    auto shut_gate() -> bool {
        auto _ = mm::set_free_memory_signal(0, NO_BOUND, NO_BOUND, nullptr,
                                            mm::DelayedAllocations::Wait);
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
        refuse();
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
        auto const armed_low = mm::set_free_memory_signal(before - MARGIN, NO_BOUND, 0, &watched,
                                                          mm::DelayedAllocations::Wait);
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
        auto const armed_high = mm::set_free_memory_signal(0, during + MARGIN, 0, &watched,
                                                           mm::DelayedAllocations::Wait);
        for (usize i = 0; i < taken; ++i) {
            mm::free_frames(frames[i], 0);
        }
        auto const high = watched.wait(deadline_after(SETTLE)) == Status::Ok;
        verdict(tally, armed_high && high,
                "a free that takes free memory above the upper bound signals the event");
        open_gate();
    }

    /// One use of a user VMO's page, by a thread of its own: it sleeps while
    /// the gate is shut.
    struct VmoAccess {
        enum class Kind {
            /// A store of |value| to |va|, a mapping of the page: a write fault.
            Store,
            /// vmo->write() of |value| to |offset|.
            Write,
            /// vmo->commit_range() of the page at |offset|.
            Commit,
        };

        Kind kind = Kind::Store;
        mm::VmObjectPaged *vmo = nullptr;
        VirtAddr va = 0;
        u64 offset = 0;
        u64 value = 0;
        Status status = Status::InternalError;
    };

    auto access_routine(void *const arg) -> i32 {
        auto &access = *static_cast<VmoAccess *>(arg);
        switch (access.kind) {
            case VmoAccess::Kind::Store:
                access.status = user_copy::copy_to_user(access.va, &access.value, sizeof(u64));
                break;
            case VmoAccess::Kind::Write:
                access.status = access.vmo->write(&access.value, access.offset, sizeof(u64));
                break;
            case VmoAccess::Kind::Commit:
                access.status = access.vmo->commit_range(access.offset, PAGE_SIZE,
                                                         mm::VmObject::CommitOptions::None);
                break;
        }
        return 0;
    }

    /// Runs |access| in a thread of |aspace| with the gate shut, and opens it
    /// once the thread is asleep. Whether it slept, and finished once the
    /// gate opened. One that is still asleep then is woken with a refusal, so
    /// that the command never waits on it for ever.
    auto run_shut(mm::VmAspace *const aspace, VmoAccess *const access) -> bool {
        auto const shut = shut_gate();
        auto *const thread =
            Thread::spawn("mempressure-access", access_routine, access, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            open_gate();
            return false;
        }
        thread->set_aspace(aspace);
        auto _ = thread->resume();

        auto const asleep = still_waiting(thread);
        open_gate();
        i32 retcode = 0;
        if (thread->join(&retcode, deadline_after(LOST_AFTER)) == Status::Ok) {
            return shut && asleep;
        }
        log::error("mempressure.test: an access slept on after the gate opened; refusing it");
        refuse();
        auto _ = thread->join(&retcode, time::Deadline::infinite());
        open_gate();
        return false;
    }

    auto check_vmo(Tally &tally) -> void {
        constexpr u64 P = PAGE_SIZE;

        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "mempressure.test", &aspace);
            status != Status::Ok) {
            verdict(tally, false, "an address space for the VMO checks is made");
            return;
        }
        ustl::Arc<mm::VmObjectPaged> vmo;
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto const made =
            mm::VmObjectPaged::create(WAITABLE, 3 * P, mm::VmoFlags::Lazy, &vmo) == Status::Ok;
        VirtAddr base = 0;
        if (made) {
            auto mapping = aspace->root_vma()->map_with_vmo(
                mm::VmPlacement::anywhere(), 3 * P, 0, rw, mm::VmaFlags::PermMask,
                ustl::Arc<mm::VmObject>(vmo), mm::VmMapOption::None, "mempressure.test");
            if (mapping.is_ok()) {
                base = mapping.unwrap()->base();
            }
        }
        if (base == 0) {
            verdict(tally, false, "a user VMO is made and mapped");
            auto _ = aspace->destroy();
            return;
        }

        // Whether the page at |offset| reads |value| back.
        auto const holds = [&vmo](u64 const offset, u64 const value) {
            u64 got = 0;
            return vmo->read(&got, offset, sizeof(got)) == Status::Ok && got == value;
        };

        VmoAccess store{.kind = VmoAccess::Kind::Store,
                        .vmo = vmo.as_ptr_mut(),
                        .va = base,
                        .offset = 0,
                        .value = 0x5107'e000'0000'0001};
        auto const stored = run_shut(aspace.as_ptr_mut(), &store);
        verdict(tally, stored && store.status == Status::Ok && holds(0, store.value),
                "a store to a user VMO's mapping sleeps while the gate is shut, and lands once "
                "it opens");

        VmoAccess write{.kind = VmoAccess::Kind::Write,
                        .vmo = vmo.as_ptr_mut(),
                        .offset = P,
                        .value = 0x5107'e000'0000'0002};
        auto const written = run_shut(aspace.as_ptr_mut(), &write);
        verdict(tally, written && write.status == Status::Ok && holds(P, write.value),
                "a write of a user VMO sleeps while the gate is shut, and lands once it opens");

        VmoAccess commit{.kind = VmoAccess::Kind::Commit, .vmo = vmo.as_ptr_mut(), .offset = 2 * P};
        auto const committed = run_shut(aspace.as_ptr_mut(), &commit);
        verdict(tally, committed && commit.status == Status::Ok && holds(2 * P, 0),
                "a commit of a user VMO sleeps while the gate is shut, and commits once it opens");

        verdict(tally, aspace->destroy() == Status::Ok,
                "the address space of the VMO checks is destroyed");
    }

    /// Whether the watchdog declares |expected| within LOST_AFTER.
    auto settles_at(mm::MemoryPressure const expected) -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (mm::MemoryWatchdog::get().pressure() != expected) {
            if (time::SteadyClock::now() >= give_up) {
                log::error("mempressure.test: the watchdog says {}, not {}",
                           mm::to_string(mm::MemoryWatchdog::get().pressure()),
                           mm::to_string(expected));
                return false;
            }
            auto _ = Thread::Current::sleep_for(Milliseconds(5), task::Interruptible::No);
        }
        return true;
    }

    auto check_watchdog(Tally &tally) -> void {
        using mm::MemoryPressure;
        using mm::MemoryWatchdog;
        // These move the levels to where free memory is and look at what the
        // watchdog decides; what it has the reclaimer give back would move
        // free memory off them (pagereclaim.test checks that).
        mm::MemoryReclaimer::Pause no_reclaim{mm::MemoryReclaimer::get()};
        constexpr usize K = NR_CROSSING;
        /// A debounce of a few frames, and no hysteresis, so that each move
        /// shows at once.
        constexpr usize DEBOUNCE = 8;
        auto const at = [](usize const out_of_memory, usize const critical, usize const warning) {
            return mm::MemoryPressureLevels::make(out_of_memory, critical, warning, DEBOUNCE,
                                                  Duration::zero());
        };
        auto &watchdog = MemoryWatchdog::get();
        auto const boot = mm::MemoryPressureLevels::from_boot_options();
        verdict(tally, settles_at(MemoryPressure::Normal),
                "with the boot levels, the watchdog finds Normal pressure");

        auto const free = mm::approx_free_frame_count();
        auto const moved_levels = at(free - 3 * K, free - K, free + K);
        auto const moved = moved_levels && watchdog.set_levels(*moved_levels) == Status::Ok &&
                           settles_at(MemoryPressure::Warning);
        verdict(tally, moved,
                "with free memory moved under the Warning level, the pressure is Warning");

        std::array<mm::PmFrame *, 2 * K> frames{};
        usize taken = 0;
        for (auto &frame: frames) {
            auto result = mm::try_alloc_frames(mm::GAF_USER, 0);
            if (!result) {
                break;
            }
            frame = *result;
            taken += 1;
        }
        auto const critical = settles_at(MemoryPressure::Critical);
        for (usize i = 0; i < taken; ++i) {
            mm::free_frames(frames[i], 0);
        }
        auto const back = settles_at(MemoryPressure::Warning);
        verdict(tally, taken == 2 * K && critical && back,
                "allocations past the Critical level make it Critical, and the frees bring it "
                "back to Warning");

        auto const now = mm::approx_free_frame_count();
        auto const oom_levels = at(now + K, now + 3 * K, now + 5 * K);
        auto const declared = oom_levels && watchdog.set_levels(*oom_levels) == Status::Ok &&
                              settles_at(MemoryPressure::OutOfMemory);
        auto refused = mm::try_alloc_frames(WAITABLE, 0);
        auto served = mm::try_alloc_frames(mm::GAF_USER, 0);
        verdict(tally, declared && !refused && refused.unwrap_err() == Status::OutOfMem && !!served,
                "at OutOfMemory an allocation that could wait is refused, and one that could not "
                "is served");
        if (refused) {
            mm::free_frames(*refused, 0);
        }
        if (served) {
            mm::free_frames(*served, 0);
        }

        auto const restored = watchdog.set_levels(boot);
        auto const normal = restored == Status::Ok && settles_at(MemoryPressure::Normal);
        auto waited = mm::try_alloc_frames(WAITABLE, 0);
        verdict(tally, normal && !!waited,
                "with the boot levels back, the pressure is Normal and an allocation that could "
                "wait is served");
        if (waited) {
            mm::free_frames(*waited, 0);
        }
    }

    auto mempressure_test_command(int, char **) -> int {
        Tally tally;
        {
            // These drive the gate and the signal by hand.
            mm::MemoryWatchdog::Hold hold{mm::MemoryWatchdog::get()};
            check_gate(tally);
            check_waiter(tally);
            check_refusal(tally);
            check_signal(tally);
            check_vmo(tally);
            open_gate();
        }
        check_watchdog(tally);

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
             "check the PMM's gate for allocations that can wait, its free-memory signal, and "
             "the memory watchdog");
