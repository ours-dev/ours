/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// MemoryPressure.TheGateTheSignalAndTheWatchdog: the PMM's gate in front of
// the allocations that can wait, the free-memory signal for whoever watches
// memory, and the VMOs that wait at the gate, and the memory watchdog
// (.agents/tasks/memory-pressure).
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
// Why an in-kernel test: the gate counts the real zones' free frames, and a
// waiter is a real thread asleep on the real event.
#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/memory_reclaimer.hpp>
#include <ours/mm/memory_watchdog.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/sync/event.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>
#include <atomic>

#include <arch/spin_hint.hpp>
#include <ktest/ktest.hpp>
#include <logz4/log.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    using task::Thread;

    constexpr auto WAITABLE = mm::ALLOC_USER_VMO;
    constexpr auto NO_BOUND = ~usize{0};

    /// How long a waiter is given to fall asleep, and an event to be
    /// signalled, before the check looks.
    constexpr auto SETTLE = Milliseconds(200);
    /// How long one wait of a waiter lasts: a waiter the gate never wakes
    /// gives up after it rather than holding the test for ever.
    constexpr auto LOST_AFTER = Seconds(5);

    /// Frames taken, or given back, to move free memory across a bound,
    /// against a bound this many frames away.
    constexpr usize NR_CROSSING = 64;
    constexpr usize MARGIN = NR_CROSSING / 2;

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
        auto const delayed = mm::alloc_frames(WAITABLE);
        if (delayed) {
            mm::free_frames(*delayed);
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
            auto result = mm::wait_for_frame_allocation(WAITABLE, deadline_after(LOST_AFTER),
                                                        task::Interruptible::No);
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

    auto check_gate() -> void {
        open_gate();
        auto served = mm::alloc_frames(WAITABLE);
        EXPECT_TRUE(!!served, "with no delay level, a CanWait allocation is served");
        if (served) {
            mm::free_frames(*served);
        }

        auto const shut = shut_gate();
        auto plain = mm::alloc_frames(mm::ALLOC_USER);
        EXPECT_TRUE(
            shut && !!plain,
            "below the delay level a CanWait allocation answers ShouldWait, and one without "
            "CanWait is served");
        if (plain) {
            mm::free_frames(*plain);
        }

        auto const by_node = mm::alloc_frames(WAITABLE, 0, mm::current_node());
        EXPECT_TRUE(!by_node && by_node.unwrap_err() == Status::ShouldWait,
                    "an allocation that names its home shares pressure admission");
        if (by_node) {
            mm::free_frames(*by_node);
        }
    }

    auto check_waiter() -> void {
        auto const shut = shut_gate();
        Waiter waiter;
        auto *const thread = start(&waiter);
        if (thread == nullptr) {
            open_gate();
            FAIL("start a waiter thread");
        }
        auto const asleep = still_waiting(thread);
        open_gate();
        auto const ended = finish(thread);
        EXPECT_TRUE(shut && asleep && ended && waiter.status == Status::Ok &&
                        waiter.frame != nullptr,
                    "a waiter sleeps while the gate is shut, and wakes with a frame when it opens");
        if (waiter.frame != nullptr) {
            mm::free_frames(waiter.frame);
        }
    }

    auto check_refusal() -> void {
        auto const shut = shut_gate();
        Waiter waiter;
        auto *const thread = start(&waiter);
        if (thread == nullptr) {
            open_gate();
            FAIL("start a waiter thread");
        }
        auto const asleep = still_waiting(thread);
        refuse();
        auto const ended = finish(thread);
        auto refused = mm::alloc_frames(WAITABLE);
        EXPECT_TRUE(shut && asleep && ended && waiter.status == Status::OutOfMem &&
                        waiter.frame == nullptr && !refused &&
                        refused.unwrap_err() == Status::OutOfMem,
                    "with delayed allocations refused, a waiter wakes with OutOfMem, and a CanWait "
                    "allocation below the level fails at once");
        if (waiter.frame != nullptr) {
            mm::free_frames(waiter.frame);
        }
        if (refused) {
            mm::free_frames(*refused);
        }
        open_gate();
    }

    auto check_timeout() -> void {
        auto const shut = shut_gate();
        auto const deadline = deadline_after(SETTLE);
        auto result = mm::wait_for_frame_allocation(WAITABLE, deadline, task::Interruptible::Yes);
        if (!result && result.unwrap_err() == Status::ShouldWait) {
            // Closing the gate can leave a wakeup from its previous opening.
            result = mm::wait_for_frame_allocation(WAITABLE, deadline, task::Interruptible::Yes);
        }
        open_gate();
        EXPECT_TRUE(shut && !result && result.unwrap_err() == Status::TimeOut,
                    "an interruptible allocation wait times out while the gate stays shut");
        if (result) {
            mm::free_frames(*result);
        }
    }

    auto check_signal() -> void {
        sync::Event watched;
        sync::Event rejected;
        // Free memory pushed below the lower bound by allocations.
        auto const before = mm::approx_free_frame_count();
        auto const armed_low = mm::set_free_memory_signal(before - MARGIN, NO_BOUND, 0, &watched,
                                                          mm::DelayedAllocations::Wait);
        auto const stale_low = mm::set_free_memory_signal(NO_BOUND, NO_BOUND, 0, &rejected,
                                                          mm::DelayedAllocations::Wait);
        auto const stale_high =
            mm::set_free_memory_signal(0, 0, 0, &rejected, mm::DelayedAllocations::Wait);
        // The first fragment crosses the lower bound before the second fails.
        constexpr auto ROLLBACK = NR_CROSSING | (usize{1} << (MAX_FRAME_ORDER + 1));
        auto rolled_back = mm::alloc_frames_fragment(mm::ALLOC_KERNEL, ROLLBACK);
        auto const invalid = !rolled_back && rolled_back.unwrap_err() == Status::InvArgs;
        if (rolled_back) {
            mm::free_frames(&*rolled_back);
        }
        auto const quiet = watched.wait(deadline_after(SETTLE)) == Status::TimeOut;
        EXPECT_TRUE(armed_low && invalid && quiet,
                    "a partial withdrawal is rolled back before pressure is sampled");

        auto batch = mm::alloc_frames_bulk(mm::ALLOC_USER, NR_CROSSING);
        auto const low = watched.wait(deadline_after(SETTLE)) == Status::Ok;
        EXPECT_TRUE(armed_low && batch && batch->size() == NR_CROSSING && low,
                    "a batch below the lower bound signals the event");
        watched.unsignal();

        // And above the upper bound by frees.
        auto const during = mm::approx_free_frame_count();
        auto const armed_high = mm::set_free_memory_signal(0, during + MARGIN, 0, &watched,
                                                           mm::DelayedAllocations::Wait);
        if (batch) {
            mm::free_frames(&*batch);
        }
        auto const high = watched.wait(deadline_after(SETTLE)) == Status::Ok;
        EXPECT_TRUE(armed_high && high, "a list return above the upper bound signals the event");
        open_gate();
        EXPECT_TRUE(!stale_low && !stale_high && low,
                    "stale bounds are rejected without replacing the armed event");
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
        VirtAddr va{};
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
    /// that the test never waits on it for ever.
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
        log::error("mempressure: an access slept on after the gate opened; refusing it");
        refuse();
        auto _ = thread->join(&retcode, time::Deadline::infinite());
        open_gate();
        return false;
    }

    auto check_vmo() -> void {
        constexpr u64 P = PAGE_SIZE;

        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "mempressure.test", &aspace);
            status != Status::Ok) {
            FAIL("make an address space for the VMO checks");
        }
        ustl::Arc<mm::VmObjectPaged> vmo;
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto const made =
            mm::VmObjectPaged::create(WAITABLE, 3 * P, mm::VmoFlags::Lazy, &vmo) == Status::Ok;
        VirtAddr base{};
        if (made) {
            auto mapping = aspace->root_vma()->map_vmo(
                VirtAddr::Anywhere, 3 * P, 0, rw, mm::VmaFlags::PermMask,
                ustl::Arc<mm::VmObject>(vmo), mm::MapOptions::Lazy, "mempressure.test");
            if (mapping.is_ok()) {
                base = mapping.unwrap()->base();
            }
        }
        if (base == VirtAddr{0}) {
            auto _ = aspace->destroy();
            FAIL("make and map a user VMO");
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
        EXPECT_TRUE(stored && store.status == Status::Ok && holds(0, store.value),
                    "a store to a user VMO's mapping sleeps while the gate is shut, and lands once "
                    "it opens");

        VmoAccess write{.kind = VmoAccess::Kind::Write,
                        .vmo = vmo.as_ptr_mut(),
                        .offset = P,
                        .value = 0x5107'e000'0000'0002};
        auto const written = run_shut(aspace.as_ptr_mut(), &write);
        EXPECT_TRUE(written && write.status == Status::Ok && holds(P, write.value),
                    "a write of a user VMO sleeps while the gate is shut, and lands once it opens");

        VmoAccess commit{.kind = VmoAccess::Kind::Commit, .vmo = vmo.as_ptr_mut(), .offset = 2 * P};
        auto const committed = run_shut(aspace.as_ptr_mut(), &commit);
        EXPECT_TRUE(committed && commit.status == Status::Ok && holds(2 * P, 0),
                    "a commit of a user VMO sleeps while the gate is shut, and commits once it "
                    "opens");

        EXPECT_TRUE(aspace->destroy() == Status::Ok,
                    "the address space of the VMO checks is destroyed");
    }

    /// Whether the watchdog declares |expected| within LOST_AFTER.
    auto settles_at(mm::MemoryPressure const expected) -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (mm::MemoryWatchdog::get().pressure() != expected) {
            if (time::SteadyClock::now() >= give_up) {
                log::error("mempressure: the watchdog says {}, not {}",
                           mm::to_string(mm::MemoryWatchdog::get().pressure()),
                           mm::to_string(expected));
                return false;
            }
            auto _ = Thread::Current::sleep_for(Milliseconds(5), task::Interruptible::No);
        }
        return true;
    }

    auto check_watchdog() -> void {
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
        EXPECT_TRUE(settles_at(MemoryPressure::Normal),
                    "with the boot levels, the watchdog finds Normal pressure");

        auto const free = mm::approx_free_frame_count();
        auto const moved_levels = at(free - 3 * K, free - K, free + K);
        auto const moved = moved_levels && watchdog.set_levels(*moved_levels) == Status::Ok &&
                           settles_at(MemoryPressure::Warning);
        EXPECT_TRUE(moved,
                    "with free memory moved under the Warning level, the pressure is Warning");

        std::array<mm::PmFrame *, 2 * K> frames{};
        usize taken = 0;
        for (auto &frame: frames) {
            auto result = mm::alloc_frames(mm::ALLOC_USER);
            if (!result) {
                break;
            }
            frame = *result;
            taken += 1;
        }
        auto const critical = settles_at(MemoryPressure::Critical);
        for (usize i = 0; i < taken; ++i) {
            mm::free_frames(frames[i]);
        }
        auto const back = settles_at(MemoryPressure::Warning);
        EXPECT_TRUE(taken == 2 * K && critical && back,
                    "allocations past the Critical level make it Critical, and the frees bring it "
                    "back to Warning");

        auto const now = mm::approx_free_frame_count();
        auto const oom_levels = at(now + K, now + 3 * K, now + 5 * K);
        auto const declared = oom_levels && watchdog.set_levels(*oom_levels) == Status::Ok &&
                              settles_at(MemoryPressure::OutOfMemory);
        auto refused = mm::alloc_frames(WAITABLE);
        auto served = mm::alloc_frames(mm::ALLOC_USER);
        EXPECT_TRUE(declared && !refused && refused.unwrap_err() == Status::OutOfMem && !!served,
                    "at OutOfMemory an allocation that could wait is refused, and one that could "
                    "not is served");
        if (refused) {
            mm::free_frames(*refused);
        }
        if (served) {
            mm::free_frames(*served);
        }

        auto const restored = watchdog.set_levels(boot);
        auto const normal = restored == Status::Ok && settles_at(MemoryPressure::Normal);
        auto waited = mm::alloc_frames(WAITABLE);
        EXPECT_TRUE(normal && !!waited,
                    "with the boot levels back, the pressure is Normal and an allocation that "
                    "could wait is served");
        if (waited) {
            mm::free_frames(*waited);
        }
    }

    KTEST(MemoryPressure, TheGateTheSignalAndTheWatchdog) {
        {
            // These drive the gate and the signal by hand.
            mm::MemoryWatchdog::Hold hold{mm::MemoryWatchdog::get()};
            check_gate();
            check_waiter();
            check_refusal();
            check_timeout();
            check_signal();
            check_vmo();
            open_gate();
        }
        check_watchdog();
    }

    /// One CPU admitting allocations that can wait, as fast as it can, while
    /// another re-arms the gate under it. |attempts| counts the allocations
    /// it has finished, so the re-arming side can tell when one it may have
    /// raced has landed.
    struct Admitter {
        std::atomic<bool> stop{false};
        std::atomic<usize> attempts{0};
    };

    auto admit_routine(void *const arg) -> i32 {
        auto &admitter = *static_cast<Admitter *>(arg);
        while (!admitter.stop.load(std::memory_order_acquire)) {
            auto const result = mm::alloc_frames(WAITABLE);
            if (result) {
                mm::free_frames(*result);
            }
            admitter.attempts.fetch_add(1, std::memory_order_release);
        }
        return 0;
    }

    /// Spins until |admitter| has finished |more| allocations after now.
    /// False if it has not within LOST_AFTER: the admitter is not running.
    auto wait_for_attempts(Admitter const &admitter, usize const more) -> bool {
        auto const target = admitter.attempts.load(std::memory_order_acquire) + more;
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (admitter.attempts.load(std::memory_order_acquire) < target) {
            if (time::SteadyClock::now() >= give_up) {
                return false;
            }
            arch::spin_hint();
        }
        return true;
    }

    KTEST(MemoryPressure, TheGateIsNeverShutOnALevelNoLongerInForce) {
        // Each round arms a level no free count reaches, so the admitter's
        // next allocation decides to shut the gate, and then at once no
        // level at all. An allocation that read the first level but shut the
        // gate after the second was stored has shut it on a level no longer
        // in force: the gate stays shut with nothing to open it. After the
        // second store the main thread waits for the admitter's allocation
        // in flight and one more to finish, so any such shut has landed, and
        // then asks: with no level, only a stale shut answers ShouldWait.
        //
        // A race, so a pass is evidence rather than proof; it needs a second
        // CPU for the admitter to race on at all.
        mm::MemoryWatchdog::Hold hold{mm::MemoryWatchdog::get()};
        Admitter admitter;
        auto *const thread =
            Thread::spawn("mempressure-admitter", admit_routine, &admitter, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            FAIL("a thread for the admitter");
            return;
        }
        if (Status::Ok != thread->resume()) {
            FAIL("the admitter starts");
            return;
        }

        // The probe is confined to this CPU's node, so a zone below its Low
        // watermark cannot answer it ShouldWait (it goes on to Min): only a
        // shut gate can.
        constexpr usize ROUNDS = 40000;
        usize stale = 0;
        auto running = true;
        for (usize round = 0; round < ROUNDS && running; ++round) {
            auto _ = mm::set_free_memory_signal(0, NO_BOUND, NO_BOUND, nullptr,
                                                mm::DelayedAllocations::Wait);
            auto _ =
                mm::set_free_memory_signal(0, NO_BOUND, 0, nullptr, mm::DelayedAllocations::Wait);
            running = wait_for_attempts(admitter, 2);
            auto const probe = mm::alloc_frames(WAITABLE | mm::Gaf::ThisNode);
            if (probe) {
                mm::free_frames(*probe);
            } else if (probe.unwrap_err() == Status::ShouldWait) {
                stale += 1;
            }
        }

        // The admitter reads |admitter| on this stack until it stops, so it
        // is joined for as long as that takes; its loop never sleeps.
        admitter.stop.store(true, std::memory_order_release);
        i32 retcode = 0;
        auto _ = thread->join(&retcode, time::Deadline::infinite());
        open_gate();
        log::info("pressure:   {} rounds, {} admissions, {} stale shuts", ROUNDS,
                  admitter.attempts.load(std::memory_order_relaxed), stale);

        EXPECT_TRUE(running, "the admitter keeps admitting while the gate is re-armed");
        EXPECT_TRUE(stale == 0, "the gate is never left shut on a level no longer in force");
    }

    KTEST(MemoryPressure, TheBootLevelsCoverTheZoneFloors) {
        // The arithmetic, on levels small enough to read: OutOfMemory 1000,
        // Critical 2000, Warning 3000, a debounce of 10, so a delay level of
        // 990.
        auto const given = mm::MemoryPressureLevels::make(1000, 2000, 3000, 10, Duration::zero());
        if (!given) {
            FAIL("the small levels nest");
            return;
        }
        auto const below = given->covering(500);
        auto const edge = given->covering(990);
        auto const far = given->covering(5000);
        EXPECT_TRUE(below.out_of_memory() == 1000 && below.warning() == 3000,
                    "floors below the delay level leave the levels alone");
        EXPECT_TRUE(edge.delay_level() == 991 && edge.critical() == 2001 && edge.warning() == 3001,
                    "floors at the delay level move every level up by one frame");
        EXPECT_TRUE(far.is_valid() && far.delay_level() == 5001 &&
                        far.critical() - far.out_of_memory() == 1000 &&
                        far.warning() - far.critical() == 1000,
                    "floors far above it move every level up, each band as wide as it was");

        // This machine: what the boot options give and what is in force both
        // keep the delay level above what the zones' floors hold.
        auto const held = mm::zone_floor_frame_count();
        auto const boot = mm::MemoryPressureLevels::from_boot_options();
        auto const in_force = mm::MemoryWatchdog::get().levels();
        // delay_level() asserts that the levels nest, so validity is asked
        // first, and the log prints the parts.
        log::info("pressure:   the zones' floors hold {} frames; OutOfMemory {} at boot, {} in "
                  "force, debounce {}",
                  held, boot.out_of_memory(), in_force.out_of_memory(), boot.debounce());
        EXPECT_TRUE(boot.is_valid() && boot.delay_level() > held,
                    "the boot levels keep the delay level above the zones' floors");
        EXPECT_TRUE(in_force.is_valid() && in_force.delay_level() > held,
                    "so do the levels the watchdog is running with");
    }
} // namespace
} // namespace ours
