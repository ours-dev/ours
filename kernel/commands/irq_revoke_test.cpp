/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `irqrevoke.test`: the kernel taking a line away from its actions
// (irq::Desc::mark_dying(), revoke(), unmark_dying(); .agents/tasks/
// irq-multi-arch/PLAN.md, step 9). dbg image only: it is the one with a
// kernel shell. The lines are descriptors of the test's own, outside the line
// table, bound to a chip that only counts; the interrupt objects on a real
// line are interruptline.test's. What it checks:
//
//   - revoke() tells every action, once, with the line's number, while the
//     action is still on that line as its owner sees it, and takes each off:
//     detach() then answers NotFound, nothing handles an interrupt on the
//     line, and the line is masked once;
//   - a line marked dying refuses attach() (both forms), unmask() and
//     configure() with NoResource before the chip is touched, and still
//     masks; unmark_dying() gives it back;
//   - a Permanent line, and a line with an action that gave no cancel hook,
//     refuse the mark with AlreadyBound and are left as they were;
//   - revoke() is a barrier: with a handler of the line in flight on another
//     CPU, it returns only after the handler has, and the hook is told after;
//   - the race the barrier is for: an owner detaching on one CPU while the
//     kernel revokes on another. A detach that answers NotFound has been
//     told by the revoke that the hook is done with the owner's object. This
//     needs two CPUs.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/irq/desc.hpp>
#include <ours/status.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>

#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using task::Thread;

    constexpr auto OWN_LINE = irq::INVALID_V_IRQ_NUM - 1;
    constexpr auto LOST_AFTER = Seconds(5);
    /// How long the slow handler holds its line.
    constexpr auto HOLD_FOR = Milliseconds(20);
    constexpr u32 RACE_ROUNDS = 400;
    constexpr u32 RACE_DELAY_SPINS = 1500;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
        usize skipped = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("irqrevoke.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("irqrevoke.test: FAIL  {}", name);
    }

    // NOLINTBEGIN(readability-identifier-naming)
    std::atomic<u64> MASKS{0};
    std::atomic<u64> UNMASKS{0};
    std::atomic<u64> CONFIGURES{0};
    // NOLINTEND(readability-identifier-naming)

    auto count_mask(irq::Data const &) -> void {
        MASKS.fetch_add(1, std::memory_order_relaxed);
    }

    auto count_unmask(irq::Data const &) -> void {
        UNMASKS.fetch_add(1, std::memory_order_relaxed);
    }

    auto count_configure(irq::Data const &, irq::TriggerMode, irq::Polarity) -> Status {
        CONFIGURES.fetch_add(1, std::memory_order_relaxed);
        return Status::Ok;
    }

    auto edge_mode(irq::Data const &) -> irq::TriggerMode {
        return irq::TriggerMode::Edge;
    }

    auto no_eoi(irq::Data const &) -> void {}

    constexpr irq::Chip CHIP = {
        .mask = count_mask,
        .unmask = count_unmask,
        .configure = count_configure,
        .trigger_mode = edge_mode,
        .issue_eoi = no_eoi,
        .name = "irq-revoke-test",
    };

    /// A handler in flight that the test can see: set by whoever holds the
    /// line for a while.
    struct Hold {
        std::atomic<bool> entered{false};
        std::atomic<bool> returned{false};
    };

    auto owner_handler(VIrqNum line, void *context) -> irq::IrqReturn;
    auto owner_cancel(VIrqNum line, void *context) -> void;

    /// An action and what its owner sees of it.
    struct Owner {
        Owner(irq::IrqFlags const flags, bool const revocable)
            : action(&owner_handler, this, flags, "irq-revoke-test",
                     revocable ? &owner_cancel : nullptr) {}

        irq::Action action;
        Hold *hold = nullptr;
        std::atomic<u32> handled{0};
        std::atomic<u32> told{0};
        /// What the hook found: the number it was given, whether the action
        /// was still on that line as the owner reads it, and whether the
        /// handler in flight had returned.
        std::atomic<VIrqNum> told_with{irq::INVALID_V_IRQ_NUM};
        std::atomic<bool> on_line_in_hook{false};
        std::atomic<bool> handler_returned_in_hook{false};
        /// The hook's last act, with release: whoever reads it true may
        /// free the owner.
        std::atomic<bool> hook_done{false};
    };

    auto owner_handler(VIrqNum, void *const context) -> irq::IrqReturn {
        auto &owner = *static_cast<Owner *>(context);
        owner.handled.fetch_add(1, std::memory_order_relaxed);
        if (owner.hold != nullptr) {
            owner.hold->entered.store(true, std::memory_order_release);
            auto const until = time::SteadyClock::now() + HOLD_FOR;
            while (time::SteadyClock::now() < until) {
                arch::spin_hint();
            }
            owner.hold->returned.store(true, std::memory_order_release);
        }
        return irq::IrqReturn::Handled;
    }

    auto owner_cancel(VIrqNum const line, void *const context) -> void {
        auto &owner = *static_cast<Owner *>(context);
        owner.told_with.store(line, std::memory_order_relaxed);
        owner.on_line_in_hook.store(owner.action.virqnum() == line, std::memory_order_relaxed);
        if (owner.hold != nullptr) {
            owner.handler_returned_in_hook.store(
                owner.hold->returned.load(std::memory_order_acquire), std::memory_order_relaxed);
        }
        owner.told.fetch_add(1, std::memory_order_relaxed);
        owner.hook_done.store(true, std::memory_order_release);
    }

    /// A line of the test's own; null if it cannot be made. The caller
    /// deletes it with nobody on it.
    auto own_line(irq::IrqFlags const flags = irq::IrqFlags::None) -> irq::Desc * {
        auto *const line = irq::Desc::create(OWN_LINE, OWN_LINE, flags, "irq-revoke-test");
        if (line == nullptr) {
            return nullptr;
        }
        if (line->init(CHIP) != Status::Ok) {
            delete line;
            return nullptr;
        }
        return line;
    }

    auto spawn_pinned(char const *const name, Thread::StartRoutine const routine, void *const arg,
                      CpuNum const cpu) -> Thread * {
        auto *const thread = Thread::spawn(name, routine, arg, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return nullptr;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        // A thread that does not start is found by its join, which times out.
        auto _ = thread->resume();
        return thread;
    }

    auto join_within(Thread *const thread) -> bool {
        return thread != nullptr &&
               thread->join(nullptr, time::Deadline::at(time::SteadyClock::now() + LOST_AFTER)) ==
                   Status::Ok;
    }

    auto check_tells_every_action(Tally &tally) -> void {
        auto *const line = own_line();
        if (line == nullptr) {
            verdict(tally, false, "make the line");
            return;
        }
        Owner first(irq::IrqFlags::Shared, true);
        Owner second(irq::IrqFlags::Shared, true);
        auto const attached =
            line->attach(first.action) == Status::Ok && line->attach(second.action) == Status::Ok;
        verdict(tally, attached, "two shared actions with hooks take the line");

        auto const masks_before = MASKS.load(std::memory_order_relaxed);
        auto const marked = line->mark_dying();
        line->revoke();
        auto const masks = MASKS.load(std::memory_order_relaxed) - masks_before;

        verdict(tally, marked == Status::Ok, "the line is marked");
        verdict(tally,
                first.told.load() == 1 && second.told.load() == 1 &&
                    first.told_with.load() == OWN_LINE && second.told_with.load() == OWN_LINE,
                "each action is told once, with the line's number");
        verdict(tally, first.on_line_in_hook.load() && second.on_line_in_hook.load(),
                "and was still on that line, as its owner reads it, while it was told");
        verdict(tally, !first.action.is_attached() && !second.action.is_attached(),
                "then both are off the line");
        verdict(tally,
                line->detach(first.action) == Status::NotFound &&
                    line->detach(second.action) == Status::NotFound,
                "and a detach answers NotFound");
        verdict(tally, masks == 1, "the line is masked, once");
        verdict(tally,
                line->handle_irq(irq::Eoi::Skip) == irq::IrqReturn::None &&
                    first.handled.load() == 0 && second.handled.load() == 0,
                "and no action handles an interrupt on it");
        delete line;
    }

    auto check_dying_refuses(Tally &tally) -> void {
        auto *const line = own_line();
        if (line == nullptr) {
            verdict(tally, false, "make the line");
            return;
        }
        Owner owner(irq::IrqFlags::None, true);
        auto const marked = line->mark_dying();

        auto const configures = CONFIGURES.load(std::memory_order_relaxed);
        auto const unmasks = UNMASKS.load(std::memory_order_relaxed);
        auto const plain = line->attach(owner.action);
        auto const with_mode = line->attach(
            owner.action,
            ustl::Option<irq::LineMode>{irq::LineMode{.trigger = irq::TriggerMode::Edge,
                                                      .polarity = irq::Polarity::ActiveHigh}});
        auto const unmasked = line->unmask();
        auto const configured = line->configure(irq::TriggerMode::Edge, irq::Polarity::ActiveHigh);

        verdict(tally, marked == Status::Ok, "an empty line is marked");
        verdict(tally,
                plain == Status::NoResource && with_mode.is_err() &&
                    with_mode.unwrap_err() == Status::NoResource && !owner.action.is_attached(),
                "attach(), with a mode or without, answers NoResource");
        verdict(tally, unmasked == Status::NoResource && configured == Status::NoResource,
                "so do unmask() and configure()");
        verdict(tally,
                CONFIGURES.load(std::memory_order_relaxed) == configures &&
                    UNMASKS.load(std::memory_order_relaxed) == unmasks,
                "without the chip being asked to configure or unmask");
        verdict(tally, line->mask() == Status::Ok, "mask() still works");

        line->unmark_dying();
        verdict(tally, line->attach(owner.action) == Status::Ok && line->unmask() == Status::Ok,
                "unmark_dying() gives the line back");
        auto _ = line->detach(owner.action); // Best effort: the check above already failed.
        delete line;
    }

    /// The Permanent line and its owner are leaked, one pair per run: the
    /// action can never come off.
    auto check_refuses_permanent(Tally &tally) -> void {
        auto *const line = own_line();
        auto *const owner = new (mm::GAF_KERNEL) Owner(irq::IrqFlags::Permanent, true);
        if (line == nullptr || owner == nullptr) {
            verdict(tally, false, "make the Permanent line");
            return;
        }
        auto const attached = line->attach(owner->action);
        verdict(tally, attached == Status::Ok, "a Permanent action takes the line");
        verdict(tally,
                line->check_revocable() == Status::AlreadyBound &&
                    line->mark_dying() == Status::AlreadyBound,
                "a Permanent line refuses the mark, even with a hook");
        verdict(tally, owner->told.load() == 0 && owner->action.is_attached(),
                "and its action is left on it, untold");
    }

    auto check_refuses_hookless(Tally &tally) -> void {
        auto *const line = own_line();
        if (line == nullptr) {
            verdict(tally, false, "make the line");
            return;
        }
        Owner hooked(irq::IrqFlags::Shared, true);
        Owner hookless(irq::IrqFlags::Shared, false);
        auto const attached = line->attach(hooked.action) == Status::Ok &&
                              line->attach(hookless.action) == Status::Ok;
        verdict(tally, attached, "a shared line takes an action with a hook and one without");

        auto const unmasks = UNMASKS.load(std::memory_order_relaxed);
        verdict(tally,
                line->check_revocable() == Status::AlreadyBound &&
                    line->mark_dying() == Status::AlreadyBound,
                "the one without refuses the mark");
        verdict(tally,
                line->unmask() == Status::Ok &&
                    UNMASKS.load(std::memory_order_relaxed) == unmasks + 1,
                "and the refusal left no mark: the line still unmasks");

        auto const dropped = line->detach(hookless.action);
        verdict(tally, dropped == Status::Ok && line->mark_dying() == Status::Ok,
                "once it is gone the line is marked");
        line->revoke();
        verdict(tally, hooked.told.load() == 1 && hookless.told.load() == 0,
                "and revoked: the hooked action is told, the one that left is not");
        delete line;
    }

    struct Dispatch {
        irq::Desc *line;
    };

    auto dispatch_routine(void *const arg) -> i32 {
        auto const result = static_cast<Dispatch *>(arg)->line->handle_irq(irq::Eoi::Skip);
        return result == irq::IrqReturn::Handled ? 0 : 1;
    }

    auto check_barrier(Tally &tally, CpuNum const dispatch_cpu) -> void {
        auto *const line = own_line();
        if (line == nullptr) {
            verdict(tally, false, "make the line");
            return;
        }
        Hold hold;
        Owner owner(irq::IrqFlags::None, true);
        owner.hold = &hold;
        auto const attached = line->attach(owner.action) == Status::Ok;
        auto const marked = line->mark_dying();

        Dispatch dispatch{line};
        auto *const thread =
            spawn_pinned("irqrevoke.test/dispatch", dispatch_routine, &dispatch, dispatch_cpu);
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (!hold.entered.load(std::memory_order_acquire) &&
               time::SteadyClock::now() < give_up) {
            auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::No);
        }
        auto const in_flight = hold.entered.load(std::memory_order_acquire);

        line->revoke();
        auto const returned_at_revoke = hold.returned.load(std::memory_order_acquire);

        verdict(tally, attached && marked == Status::Ok && in_flight,
                "a handler of the line is in flight on another CPU");
        verdict(tally, returned_at_revoke, "revoke() returns only after the handler has");
        verdict(tally, owner.handler_returned_in_hook.load() && owner.told.load() == 1,
                "and the hook is told after it");
        verdict(tally, join_within(thread), "the dispatching thread ends");
        delete line;
    }

    /// One round at a time: main builds the line and the owner and says go,
    /// the remover revokes, the owner detaches after a delay of its own.
    struct Race {
        irq::Desc *line = nullptr;
        Owner *owner = nullptr;
        std::atomic<u32> go{0};
        std::atomic<u32> remover_done{0};
        std::atomic<u32> owner_done{0};
        std::atomic<bool> stop{false};
        std::atomic<u32> delay{0};
        std::atomic<u32> revoke_delay{0};
        std::atomic<u32> violations{0};
        std::atomic<u32> owner_won{0};
        std::atomic<u32> remover_won{0};
    };
    Race RACE;

    /// Waits for the next round; false if told to stop.
    auto next_round(u32 &seen) -> bool {
        for (;;) {
            if (RACE.stop.load(std::memory_order_acquire)) {
                return false;
            }
            auto const go = RACE.go.load(std::memory_order_acquire);
            if (go != seen) {
                seen = go;
                return true;
            }
            arch::spin_hint();
        }
    }

    auto remover_routine(void *) -> i32 {
        auto seen = u32{0};
        while (next_round(seen)) {
            for (u32 i = 0; i < RACE.revoke_delay.load(std::memory_order_relaxed); ++i) {
                arch::spin_hint();
            }
            RACE.line->revoke();
            RACE.remover_done.store(seen, std::memory_order_release);
        }
        return 0;
    }

    auto detacher_routine(void *) -> i32 {
        auto seen = u32{0};
        while (next_round(seen)) {
            for (u32 i = 0; i < RACE.delay.load(std::memory_order_relaxed); ++i) {
                arch::spin_hint();
            }
            auto const status = RACE.line->detach(RACE.owner->action);
            if (status == Status::Ok) {
                RACE.owner_won.fetch_add(1, std::memory_order_relaxed);
            } else if (status == Status::NotFound &&
                       RACE.owner->hook_done.load(std::memory_order_acquire)) {
                // The revoke took the action, and its hook is done with the
                // owner: the owner may free it now.
                RACE.remover_won.fetch_add(1, std::memory_order_relaxed);
            } else {
                RACE.violations.fetch_add(1, std::memory_order_relaxed);
            }
            RACE.owner_done.store(seen, std::memory_order_release);
        }
        return 0;
    }

    auto wait_for_round(std::atomic<u32> const &done, u32 const round) -> bool {
        auto const give_up = time::SteadyClock::now() + LOST_AFTER;
        while (done.load(std::memory_order_acquire) != round) {
            if (time::SteadyClock::now() > give_up) {
                return false;
            }
            auto _ = Thread::Current::sleep_for(Microseconds(100), task::Interruptible::No);
        }
        return true;
    }

    auto check_race(Tally &tally, CpuNum const remover_cpu, CpuNum const detacher_cpu) -> void {
        RACE.stop.store(false, std::memory_order_relaxed);
        RACE.go.store(0, std::memory_order_relaxed);
        RACE.remover_done.store(0, std::memory_order_relaxed);
        RACE.owner_done.store(0, std::memory_order_relaxed);
        RACE.violations.store(0, std::memory_order_relaxed);
        RACE.owner_won.store(0, std::memory_order_relaxed);
        RACE.remover_won.store(0, std::memory_order_relaxed);

        auto *const remover =
            spawn_pinned("irqrevoke.test/revoke", remover_routine, nullptr, remover_cpu);
        auto *const detacher =
            spawn_pinned("irqrevoke.test/detach", detacher_routine, nullptr, detacher_cpu);
        auto ran = remover != nullptr && detacher != nullptr;
        for (u32 round = 1; ran && round <= RACE_ROUNDS; ++round) {
            auto *const line = own_line();
            auto *const owner = new (mm::GAF_KERNEL) Owner(irq::IrqFlags::None, true);
            if (line == nullptr || owner == nullptr) {
                ran = false;
                delete line;
                delete owner;
                break;
            }
            if (line->attach(owner->action) != Status::Ok || line->mark_dying() != Status::Ok) {
                ran = false;
            }
            RACE.line = line;
            RACE.owner = owner;
            // One side is held back, by turns, so that the detach lands
            // before, during and after the revoke.
            auto const held_back = (round * 53) % RACE_DELAY_SPINS;
            if (round % 2 == 0) {
                RACE.delay.store(held_back, std::memory_order_relaxed);
                RACE.revoke_delay.store(0, std::memory_order_relaxed);
            } else {
                RACE.delay.store(0, std::memory_order_relaxed);
                RACE.revoke_delay.store(held_back, std::memory_order_relaxed);
            }
            RACE.go.store(round, std::memory_order_release);

            auto const finished =
                wait_for_round(RACE.remover_done, round) && wait_for_round(RACE.owner_done, round);
            if (!finished) {
                // A thread is stuck: leave the line and the owner to it.
                ran = false;
                break;
            }
            delete line;
            delete owner;
        }
        RACE.stop.store(true, std::memory_order_release);
        auto const joined = join_within(remover) && join_within(detacher);

        log::info("irqrevoke.test: {} rounds, the owner's detach won {}, the revoke won {}",
                  RACE_ROUNDS, RACE.owner_won.load(), RACE.remover_won.load());
        verdict(tally, ran && joined, "every round ran and both threads ended");
        verdict(tally, RACE.violations.load() == 0,
                "a detach never answered before the revoke was done with the owner");
        verdict(tally, RACE.owner_won.load() + RACE.remover_won.load() == RACE_ROUNDS,
                "and every round ended one way or the other");
    }

    auto irq_revoke_test_command(int, char **) -> int {
        Tally tally;

        check_tells_every_action(tally);
        check_dying_refuses(tally);
        check_refuses_permanent(tally);
        check_refuses_hookless(tally);

        auto const online = Cpu::online_mask();
        if (online.count() < 2) {
            log::info("irqrevoke.test: one CPU only, the barrier and the race are skipped");
            tally.skipped += 2;
        } else {
            auto first = INVALID_CPU_NUM;
            auto second = INVALID_CPU_NUM;
            online.for_each([&first, &second](CpuNum const cpu) {
                if (first == INVALID_CPU_NUM) {
                    first = cpu;
                } else if (second == INVALID_CPU_NUM) {
                    second = cpu;
                }
            });
            check_barrier(tally, second);
            check_race(tally, first, second);
        }

        if (tally.failed != 0) {
            log::error("irqrevoke.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        if (tally.skipped != 0) {
            log::info("irqrevoke.test: ok -- {} checks, {} case(s) skipped", tally.passed,
                      tally.skipped);
            return 0;
        }
        log::info("irqrevoke.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(IrqRevokeTest, "irqrevoke.test", ours::irq_revoke_test_command,
             "the kernel taking a line away: revoke() tells each action and takes it off, a dying "
             "line refuses new work, and a detach racing a revoke never outruns it");
