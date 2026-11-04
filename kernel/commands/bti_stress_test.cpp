/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `btistress.test`: the BTI, its pins and their tokens, from several CPUs at
// once. bti.test checks what each call does; this checks that the calls do not
// break each other, which no single thread can show.
//
// Two races, each run on three CPUs:
//
//   - The three ways a token's pin ends or is looked at, on the same tokens at
//     the same time: one thread unpins each, one closes each token's last
//     handle (which quarantines the pin if nothing unpinned it first), and one
//     asks each for its address, over and over. Whichever of the first two
//     wins, the pin has to end exactly once -- unpinned, or in the quarantine,
//     never both and never neither -- and an address asked for is either right
//     or BadState. Afterwards every pin the quarantine holds has its page
//     pinned, and no other page is.
//   - pin(), release_quarantine() and the BTI's last handle: one thread pins
//     and either unpins or closes the token; one releases the quarantine again
//     and again; one closes the BTI's last handle half way through. Every pin
//     is Ok or BadState (the BTI was in a fault state, or has lost its last
//     handle), never anything else, and when the dust has settled one
//     release_quarantine() leaves nothing pinned and nothing counted.
//
// What this cannot show is that the code is right: it shows that these
// interleavings, tried many times, leave the counts consistent and do not
// panic, hang or free a record twice. The argument for why is in the change
// record; the lock-order check (lockdep) and the debug assertions in the BTI do
// the work of noticing when it is wrong.
//
// The test closes a BTI's last handle and then releases its quarantine itself,
// so the log has "leaked" warnings that are not leaks.
//
// A console command rather than a unit test for the reason mutex.stress gives.
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/cpu.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/object/bti_dispatcher.hpp>
#include <ours/object/iommu_dispatcher.hpp>
#include <ours/object/pmt_dispatcher.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>
#include <utility>

#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    constexpr usize P = PAGE_SIZE;

    /// Tokens in one pass of the first race, and passes.
    constexpr usize TOKENS = 32;
    constexpr usize PASSES = 6;

    /// Pages the second race pins, and pins it makes.
    constexpr usize PAGES = 8;
    constexpr usize PINS = 400;

    constexpr usize WORKERS = 3;
    constexpr auto STUCK_AFTER = Seconds(30);
    constexpr auto POLL_INTERVAL = Milliseconds(10);

    struct Slot {
        object::KernelHandle<object::PmtDispatcher> handle;
        ustl::Arc<object::PmtDispatcher> token;
        PhysAddr expected{};
    };

    enum class Role : u32 {
        Unpin,
        Close,
        Query,
        Pin,
        Release,
        Orphan,
    };

    struct State {
        ustl::Arc<mm::VmObjectPaged> vmo;
        object::KernelHandle<object::BtiDispatcher> bti_handle;
        ustl::Arc<object::BtiDispatcher> bti;
        Slot *slots = nullptr;

        std::atomic<usize> ready{0};
        std::atomic<bool> go{false};
        /// How many of the three have reached token i: each waits for the
        /// others there, so the three act on it together.
        std::atomic<usize> arrived[TOKENS]{};
        std::atomic<usize> finished{0};
        std::atomic<usize> violations{0};

        /// The second race: how far the pinning thread is, and whether it is done.
        std::atomic<usize> pinned_so_far{0};
        std::atomic<bool> pinning_done{false};
        std::atomic<usize> pins_ok{0};
        std::atomic<usize> pins_refused{0};
    };

    struct Worker {
        State *state;
        Role role;
    };

    // The analysis cannot tie `node->lock()` to the lock the count requires:
    // two expressions through Arc::operator->(). It is held from the guard to
    // the end.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto nr_pinned(mm::VmObjectPaged &vmo) -> usize {
        auto &node = const_cast<ustl::Arc<mm::VmCowPages> &>(vmo.cow_pages());
        Guard guard{node->lock()};
        return node->nr_pinned_pages_locked();
    }

    auto phys_of(mm::VmObjectPaged &vmo, usize const index) -> PhysAddr {
        auto pa = PhysAddr{0};
        if (vmo.lookup_contiguous(index * P, P, &pa) != Status::Ok) {
            return PhysAddr{0};
        }
        return pa;
    }

    auto pause(usize const spins) -> void {
        for (usize i = 0; i < spins; ++i) {
            arch::spin_hint();
        }
    }

    /// Starts every worker of a race together.
    auto wait_for_go(State &state) -> void {
        state.ready.fetch_add(1, std::memory_order_acq_rel);
        while (!state.go.load(std::memory_order_acquire)) {
            arch::spin_hint();
        }
    }

    auto run_first_race_role(State &state, Role const role) -> void {
        wait_for_go(state);
        for (usize i = 0; i < TOKENS; ++i) {
            auto &slot = state.slots[i];

            // Meet the others at this token, then act a little apart from them,
            // each role first on some tokens and last on others, so that every
            // order of the three is tried.
            state.arrived[i].fetch_add(1, std::memory_order_acq_rel);
            while (state.arrived[i].load(std::memory_order_acquire) < WORKERS) {
                arch::spin_hint();
            }
            pause(((i + std::to_underlying(role)) % 3) * 12);

            switch (role) {
                case Role::Unpin:
                    slot.token->unpin();
                    break;
                case Role::Close:
                    slot.handle.reset();
                    break;
                case Role::Query:
                    for (usize again = 0; again < 6; ++again) {
                        object::PmtDispatcher::Address where{};
                        auto const status = slot.token->query_address(0, P, where);
                        if (status == Status::Ok) {
                            if (where.address != std::to_underlying(slot.expected) ||
                                where.size != P) {
                                state.violations.fetch_add(1, std::memory_order_relaxed);
                            }
                        } else if (status != Status::BadState) {
                            state.violations.fetch_add(1, std::memory_order_relaxed);
                        }
                        pause(8);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    auto run_second_race_role(State &state, Role const role) -> void {
        wait_for_go(state);
        switch (role) {
            case Role::Pin:
                for (usize i = 0; i < PINS; ++i) {
                    object::KernelHandle<object::PmtDispatcher> handle;
                    auto rights = Rights::None;
                    auto const status = state.bti->pin(
                        state.vmo, (i % PAGES) * P, P,
                        BtiPinOptions::PermRead | BtiPinOptions::PermWrite, handle, rights);
                    if (status == Status::Ok) {
                        state.pins_ok.fetch_add(1, std::memory_order_relaxed);
                        if (i % 5 != 0) {
                            handle.dispatcher()->unpin();
                        }
                        // Dropped here: quarantined if it was not unpinned, one
                        // in five.
                    } else if (status == Status::BadState) {
                        state.pins_refused.fetch_add(1, std::memory_order_relaxed);
                        // Let the releasing thread get in before asking again.
                        pause(48);
                    } else {
                        state.violations.fetch_add(1, std::memory_order_relaxed);
                    }
                    state.pinned_so_far.store(i + 1, std::memory_order_release);
                }
                state.pinning_done.store(true, std::memory_order_release);
                break;
            case Role::Release:
                while (!state.pinning_done.load(std::memory_order_acquire)) {
                    state.bti->release_quarantine();
                    pause(32);
                }
                break;
            case Role::Orphan:
                while (state.pinned_so_far.load(std::memory_order_acquire) < (PINS * 3) / 4) {
                    arch::spin_hint();
                }
                state.bti_handle.reset();
                break;
            default:
                break;
        }
    }

    auto worker_routine(void *arg) -> i32 {
        auto const &worker = *static_cast<Worker const *>(arg);
        auto &state = *worker.state;
        switch (worker.role) {
            case Role::Unpin:
            case Role::Close:
            case Role::Query:
                run_first_race_role(state, worker.role);
                break;
            default:
                run_second_race_role(state, worker.role);
                break;
        }
        // Last thing this thread does with |state|.
        state.finished.fetch_add(1, std::memory_order_release);
        return 0;
    }

    /// Runs the three roles together and waits for them. False if they did not
    /// finish, in which case |state| and |workers| are left to them.
    auto run_race(State &state, Worker *const workers, char const *const name) -> bool {
        auto const num_cpus = Cpu::num_online_cpus();
        state.ready.store(0, std::memory_order_relaxed);
        state.go.store(false, std::memory_order_relaxed);
        state.finished.store(0, std::memory_order_relaxed);

        auto const started = time::SteadyClock::now();
        for (usize i = 0; i < WORKERS; ++i) {
            auto *const thread = task::Thread::spawn("bti-stress", worker_routine, &workers[i],
                                                     task::DEFAULT_PRIORITY);
            if (thread == nullptr) {
                log::warn("btistress.test: could not spawn a worker for {}", name);
                return false;
            }
            thread->set_cpu_affinity_hard(CpuMask::of(static_cast<CpuNum>((i + 1) % num_cpus)));
            auto _ = thread->detach();
            auto _ = thread->resume();
        }

        // All three are running before any of them starts, or the first would
        // finish before the others were scheduled and there would be no race.
        while (state.ready.load(std::memory_order_acquire) < WORKERS &&
               time::SteadyClock::now() < started + STUCK_AFTER) {
            auto _ = task::Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }
        state.go.store(true, std::memory_order_release);

        while (state.finished.load(std::memory_order_acquire) < WORKERS &&
               time::SteadyClock::now() < started + STUCK_AFTER) {
            auto _ = task::Thread::Current::sleep_for(POLL_INTERVAL, task::Interruptible::No);
        }
        if (state.finished.load(std::memory_order_acquire) < WORKERS) {
            log::error("btistress.test: STUCK -- {} of {} workers finished in {}",
                       state.finished.load(std::memory_order_acquire), WORKERS, name);
            return false;
        }
        return true;
    }

    auto bti_stress_command(int, char **) -> int {
        if (Cpu::num_online_cpus() < 2) {
            log::warn("btistress.test: needs at least two online CPUs, have {}",
                      Cpu::num_online_cpus());
            return 1;
        }

        // Heap, not the stack: on the failure path stuck threads still point at
        // them after this command returns, so they are leaked rather than popped.
        auto *const state = new (mm::GAF_KERNEL) State{};
        auto *const slots = new (mm::GAF_KERNEL) Slot[TOKENS]{};
        auto *const workers = new (mm::GAF_KERNEL) Worker[WORKERS]{};
        if (state == nullptr || slots == nullptr || workers == nullptr) {
            log::warn("btistress.test: out of memory");
            return 1;
        }
        state->slots = slots;

        object::KernelHandle<object::IommuDispatcher> iommu;
        auto rights = Rights::None;
        if (object::IommuDispatcher::create(IommuType::Stub, 0, iommu, rights) != Status::Ok ||
            object::BtiDispatcher::create(iommu.dispatcher(), 1, state->bti_handle, rights) !=
                Status::Ok) {
            log::error("btistress.test: could not make an IOMMU and a BTI");
            return 1;
        }
        state->bti = state->bti_handle.dispatcher();

        usize failures = 0;
        auto check = [&](bool const ok, char const *const what) {
            if (!ok) {
                failures += 1;
                log::error("btistress.test: FAIL  {}", what);
            }
        };

        // ---- the first race
        if (mm::VmObjectPaged::create(mm::GAF_USER, TOKENS * P, mm::VmoFlags::Lazy, &state->vmo) !=
            Status::Ok) {
            log::error("btistress.test: could not make a VMO");
            return 1;
        }
        auto const options = BtiPinOptions::PermRead | BtiPinOptions::PermWrite;
        for (usize pass = 0; pass < PASSES; ++pass) {
            auto pinned_all = true;
            for (usize i = 0; i < TOKENS; ++i) {
                auto &slot = slots[i];
                auto slot_rights = Rights::None;
                pinned_all = pinned_all && state->bti->pin(state->vmo, i * P, P, options,
                                                           slot.handle, slot_rights) == Status::Ok;
                slot.token = slot.handle.dispatcher();
                slot.expected = phys_of(*state->vmo, i);
            }
            check(pinned_all && state->bti->pin_count() == TOKENS, "the tokens are pinned");
            for (auto &count: state->arrived) {
                count.store(0, std::memory_order_relaxed);
            }
            workers[0] = {state, Role::Unpin};
            workers[1] = {state, Role::Close};
            workers[2] = {state, Role::Query};
            if (!run_race(*state, workers, "the tokens")) {
                return 1;
            }

            auto const quarantined = state->bti->quarantine_count();
            check(state->violations.load() == 0, "an address asked for was wrong, or not BadState");
            check(state->bti->pin_count() == quarantined,
                  "a pin is left that is not in the quarantine");
            check(nr_pinned(*state->vmo) == quarantined,
                  "the pages pinned are not the pins in the quarantine");
            state->bti->release_quarantine();
            check(state->bti->pin_count() == 0 && nr_pinned(*state->vmo) == 0,
                  "a release does not leave nothing pinned and nothing counted");
            log::info("btistress.test: pass {}: {} of {} pins ended in the quarantine", pass,
                      quarantined, TOKENS);
            for (usize i = 0; i < TOKENS; ++i) {
                slots[i].token = {};
            }
        }
        state->vmo = {};

        // ---- the second race
        if (mm::VmObjectPaged::create(mm::GAF_USER, PAGES * P, mm::VmoFlags::Lazy, &state->vmo) !=
            Status::Ok) {
            log::error("btistress.test: could not make a VMO");
            return 1;
        }
        workers[0] = {state, Role::Pin};
        workers[1] = {state, Role::Release};
        workers[2] = {state, Role::Orphan};
        if (!run_race(*state, workers, "pin, release and the last handle")) {
            return 1;
        }
        check(state->violations.load() == 0, "a pin answered neither Ok nor BadState");
        check(state->pins_ok.load() + state->pins_refused.load() == PINS,
              "a pin is unaccounted for");
        // Closing the BTI's last handle is half way, so some pins were made
        // after it, and every one of those was refused.
        check(state->pins_refused.load() > 0, "no pin was refused after the last handle closed");
        check(state->pins_ok.load() > 0, "no pin was made before the last handle closed");
        state->bti->release_quarantine();
        check(state->bti->pin_count() == 0 && nr_pinned(*state->vmo) == 0,
              "a release does not leave nothing pinned and nothing counted");
        log::info("btistress.test: {} pins made, {} refused", state->pins_ok.load(),
                  state->pins_refused.load());

        if (failures != 0) {
            log::error("btistress.test: FAILED -- {} checks", failures);
            return 1;
        }
        log::info("btistress.test: ok");
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(BtiStressTest, "btistress.test", ours::bti_stress_command,
             "race token unpin, token close and address queries, then pin against release and "
             "the BTI's last handle");
