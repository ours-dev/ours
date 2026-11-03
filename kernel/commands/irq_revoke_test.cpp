/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `irqrevoke.test`: the kernel taking a line away from its actions
// (irq::Line::mark_dying(), revoke(), unmark_dying(); .agents/tasks/
// irq-multi-arch/PLAN.md, step 9). dbg image only: it is the one with a
// kernel shell. The lines are descriptors of the test's own, outside the line
// table, bound to a chip that only counts, except in the race, which uses a
// line table of its own; the interrupt objects on a real line are
// interruptline.test's. What it checks:
//
//   - revoke() tells every action, once, with the line's number (not its
//     hardware number), in the context a hook is promised, while the action
//     is still on that line as its owner reads it, and takes each off:
//     detach() then answers NotFound, nothing handles an interrupt on the
//     line, and the line is masked once;
//   - a line marked dying refuses attach() (both forms), unmask() and
//     configure() with NoResource before the chip is touched, and still
//     masks; unmark_dying() gives it back;
//   - a Permanent line, and a line with an action that gave no cancel hook,
//     refuse the mark with AlreadyBound and are left as they were;
//   - what a revoked line is left with: a line that was Shared takes an
//     exclusive action again and then refuses a shared one, and a Oneshot
//     line that was held is not held by the next owner; a finalize on a
//     marked line is not lost, so a refused removal that unmarks the line
//     leaves it as it was;
//   - revoke() is a barrier: with a handler of the line in flight on another
//     CPU, it returns only after the handler has, and the hook is told after;
//   - the race the barrier is for: an owner detaching on one CPU while the
//     kernel revokes on another, through the line table, where a detach that
//     finds the action's line cleared answers without the line's lock. The
//     hook is slow, so the detach lands during it, and a detach that answers
//     NotFound must find the hook done. This needs two CPUs.
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/irq/domain.hpp>
#include <ours/irq/line.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/status.hpp>
#include <ours/task/sched/common.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <array>
#include <atomic>

#include <arch/interrupt.hpp>
#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/views/span.hpp>

namespace ours {
namespace {
    using task::Thread;

    /// A line's numbers differ, as they do in the table: a hook told the
    /// hardware number instead of the virq would show.
    constexpr auto OWN_HWIRQ = HIrqNum{7};
    constexpr auto OWN_LINE = irq::INVALID_V_IRQ_NUM - 1;
    constexpr auto LOST_AFTER = Seconds(5);
    /// How long the slow handler holds its line after the revoke has begun.
    constexpr auto HOLD_FOR = Milliseconds(20);
    constexpr u32 RACE_ROUNDS = 400;
    /// How far one side of the race is held back, and how long the hook
    /// takes, in spins.
    constexpr u32 RACE_DELAY_SPINS = 400;
    constexpr u32 RACE_HOOK_SPINS = 600;

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

    auto count_mask(irq::ControllerSource const &) -> void {
        MASKS.fetch_add(1, std::memory_order_relaxed);
    }

    auto count_unmask(irq::ControllerSource const &) -> void {
        UNMASKS.fetch_add(1, std::memory_order_relaxed);
    }

    auto count_configure(irq::ControllerSource const &, irq::TriggerMode, irq::Polarity) -> Status {
        CONFIGURES.fetch_add(1, std::memory_order_relaxed);
        return Status::Ok;
    }

    auto edge_mode(irq::ControllerSource const &) -> irq::TriggerMode {
        return irq::TriggerMode::Edge;
    }

    auto level_mode(irq::ControllerSource const &) -> irq::TriggerMode {
        return irq::TriggerMode::Level;
    }

    auto no_eoi(irq::ControllerSource const &) -> void {}

    constexpr irq::ControllerOps CHIP = {
        .mask = count_mask,
        .unmask = count_unmask,
        .configure = count_configure,
        .trigger_mode = edge_mode,
        .issue_eoi = no_eoi,
        .name = "irq-revoke-test",
    };

    /// The same over a pin that signals a level, so its lines run the level
    /// flow, which holds a Oneshot line masked.
    constexpr irq::ControllerOps LEVEL_CHIP = {
        .mask = count_mask,
        .unmask = count_unmask,
        .configure = count_configure,
        .trigger_mode = level_mode,
        .issue_eoi = no_eoi,
        .name = "irq-revoke-test",
    };

    /// A handler in flight that the test can see: it holds the line from the
    /// moment the revoke is announced for HOLD_FOR more.
    struct Hold {
        std::atomic<bool> entered{false};
        std::atomic<bool> revoking{false};
        std::atomic<bool> returned{false};
    };

    auto owner_handler(VIrqNum line, void *context) -> irq::IrqReturn;
    auto owner_cancel(VIrqNum line, void *context) -> void;

    /// An action and what its owner sees of it.
    struct Owner {
        Owner(irq::IrqFlags const flags, irq::IrqCancel const cancel)
            : action({.handle = &owner_handler,
                      .revoke = cancel,
                      .ready = [](void *) -> bool { return true; }},
                     this, flags, "irq-revoke-test") {}

        irq::Registration action;
        Hold *hold = nullptr;
        /// The handler answers WakeThread, so a Oneshot line is held.
        bool wake = false;
        /// How long the hook takes, in spins, before it says it is done.
        u32 hook_spins = 0;
        std::atomic<u32> handled{0};
        std::atomic<u32> told{0};
        /// What the hook found: the number it was given, whether the action
        /// was still on that line as the owner reads it, whether it ran with
        /// interrupts and preemption off as promised, and whether the
        /// handler in flight had returned.
        std::atomic<VIrqNum> told_with{irq::INVALID_V_IRQ_NUM};
        std::atomic<bool> on_line_in_hook{false};
        std::atomic<bool> irqs_off_in_hook{false};
        std::atomic<bool> preempt_off_in_hook{false};
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
            auto const give_up = time::SteadyClock::now() + LOST_AFTER;
            while (!owner.hold->revoking.load(std::memory_order_acquire) &&
                   time::SteadyClock::now() < give_up) {
                arch::spin_hint();
            }
            auto const until = time::SteadyClock::now() + HOLD_FOR;
            while (time::SteadyClock::now() < until) {
                arch::spin_hint();
            }
            owner.hold->returned.store(true, std::memory_order_release);
        }
        if (owner.wake) {
            return irq::IrqReturn::WakeThread;
        }
        return irq::IrqReturn::Handled;
    }

    auto owner_cancel(VIrqNum const line, void *const context) -> void {
        auto &owner = *static_cast<Owner *>(context);
        owner.told_with.store(line, std::memory_order_relaxed);
        owner.on_line_in_hook.store(owner.action.virqnum() == line, std::memory_order_relaxed);
        owner.irqs_off_in_hook.store(arch::irqs_disabled(), std::memory_order_relaxed);
        owner.preempt_off_in_hook.store(
            Thread::Current::preemption_state().preemption_disabled_count() > 0,
            std::memory_order_relaxed);
        if (owner.hold != nullptr) {
            owner.handler_returned_in_hook.store(
                owner.hold->returned.load(std::memory_order_acquire), std::memory_order_relaxed);
        }
        // A slow hook widens the window the race case aims at.
        for (u32 i = 0; i < owner.hook_spins; ++i) {
            arch::spin_hint();
        }
        owner.told.fetch_add(1, std::memory_order_relaxed);
        owner.hook_done.store(true, std::memory_order_release);
    }

    /// A line of the test's own; null if it cannot be made. The caller
    /// deletes it with nobody on it.
    auto own_line(irq::ControllerOps const &chip = CHIP) -> irq::Line * {
        auto *const line =
            irq::Line::create(OWN_HWIRQ, OWN_LINE, irq::IrqFlags::None, "irq-revoke-test");
        if (line == nullptr) {
            return nullptr;
        }
        if (line->init(chip) != Status::Ok) {
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
        Owner first(irq::IrqFlags::Shared, &owner_cancel);
        Owner second(irq::IrqFlags::Shared, &owner_cancel);
        auto const attached =
            line->attach(first.action) == Status::Ok && line->attach(second.action) == Status::Ok;
        verdict(tally, attached, "two shared actions with hooks take the line");

        auto const marked = line->mark_dying();
        verdict(tally, marked == Status::Ok, "the line is marked");
        if (marked != Status::Ok) {
            auto _ = line->detach(first.action); // Clean-up; the check above is the result.
            auto _ = line->detach(second.action);
            delete line;
            return;
        }

        auto const masks_before = MASKS.load(std::memory_order_relaxed);
        line->revoke();
        auto const masks = MASKS.load(std::memory_order_relaxed) - masks_before;

        verdict(tally,
                first.told.load() == 1 && second.told.load() == 1 &&
                    first.told_with.load() == OWN_LINE && second.told_with.load() == OWN_LINE,
                "each action is told once, with the line's number and not the pin's");
        verdict(tally, first.on_line_in_hook.load() && second.on_line_in_hook.load(),
                "and was still on that line, as its owner reads it, while it was told");
        verdict(tally,
                first.irqs_off_in_hook.load() && first.preempt_off_in_hook.load() &&
                    second.irqs_off_in_hook.load() && second.preempt_off_in_hook.load(),
                "with interrupts and preemption off, as a hook is promised");
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

        // What the revoked line is left with: it was Shared, and takes an
        // exclusive action as it would any empty line, then refuses a shared.
        line->unmark_dying();
        Owner exclusive(irq::IrqFlags::None, &owner_cancel);
        Owner sharer(irq::IrqFlags::Shared, &owner_cancel);
        auto const took = line->attach(exclusive.action);
        auto const refused = line->attach(sharer.action);
        verdict(tally, took == Status::Ok && refused == Status::AlreadyBound,
                "a line that was Shared takes an exclusive action and refuses a shared one");
        auto _ = line->detach(exclusive.action); // Clean-up; the answers above are the result.
        delete line;
    }

    auto check_dying_refuses(Tally &tally) -> void {
        auto *const line = own_line();
        if (line == nullptr) {
            verdict(tally, false, "make the line");
            return;
        }
        Owner owner(irq::IrqFlags::None, &owner_cancel);
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
        auto _ = line->detach(owner.action); // Clean-up; the check above is the result.
        delete line;
    }

    /// The Permanent line and its owner are leaked, one pair per run: the
    /// action can never come off.
    auto check_refuses_permanent(Tally &tally) -> void {
        auto *const line = own_line();
        auto *const owner = new (mm::GAF_KERNEL) Owner(irq::IrqFlags::Permanent, &owner_cancel);
        if (line == nullptr || owner == nullptr) {
            verdict(tally, false, "make the Permanent line");
            return;
        }
        auto const attached = line->attach(owner->action);
        verdict(tally, attached == Status::Ok, "a Permanent action takes the line");
        auto const unmasks = UNMASKS.load(std::memory_order_relaxed);
        verdict(tally,
                line->check_revocable() == Status::AlreadyBound &&
                    line->mark_dying() == Status::AlreadyBound,
                "a Permanent line refuses the mark, even with a hook");
        verdict(tally,
                line->unmask() == Status::Ok &&
                    UNMASKS.load(std::memory_order_relaxed) == unmasks + 1,
                "and the refusal left no mark: the line still unmasks");
        verdict(tally, owner->told.load() == 0 && owner->action.is_attached(),
                "and its action is left on it, untold");
    }

    auto check_refuses_hookless(Tally &tally) -> void {
        auto *const line = own_line();
        if (line == nullptr) {
            verdict(tally, false, "make the line");
            return;
        }
        Owner hooked(irq::IrqFlags::Shared, &owner_cancel);
        Owner hookless(irq::IrqFlags::Shared, nullptr);
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
        auto const marked = line->mark_dying();
        verdict(tally, dropped == Status::Ok && marked == Status::Ok,
                "once it is gone the line is marked");
        if (marked != Status::Ok) {
            auto _ = line->detach(hooked.action); // Clean-up; the check above is the result.
            delete line;
            return;
        }
        line->revoke();
        verdict(tally, hooked.told.load() == 1 && hookless.told.load() == 0,
                "and revoked: the hooked action is told, the one that left is not");
        delete line;
    }

    /// A Oneshot level line that is held: its action answered WakeThread, so
    /// the flow masked the line and left it held until a finalize.
    auto check_held_line(Tally &tally) -> void {
        auto *const line = own_line(LEVEL_CHIP);
        if (line == nullptr) {
            verdict(tally, false, "make the level line");
            return;
        }
        Owner owner(irq::IrqFlags::Oneshot, &owner_cancel);
        owner.wake = true;
        auto const attached = line->attach(owner.action);
        auto const woke = line->handle_irq(irq::Eoi::Skip);
        auto unmasks = UNMASKS.load(std::memory_order_relaxed);
        verdict(tally, attached == Status::Ok && woke == irq::IrqReturn::WakeThread,
                "a Oneshot action on a level line is woken and holds the line");
        verdict(tally,
                line->unmask() == Status::Ok && UNMASKS.load(std::memory_order_relaxed) == unmasks,
                "a held line stays masked through an unmask");

        // The removal is refused further on and rolled back, while the owner
        // acks: the finalize is not lost, and the line is not left held.
        auto const marked = line->mark_dying();
        line->finalize_oneshot(owner.action);
        line->unmark_dying();
        verdict(tally,
                marked == Status::Ok && UNMASKS.load(std::memory_order_relaxed) == unmasks + 1,
                "a finalize on a marked line unmasks it");
        unmasks = UNMASKS.load(std::memory_order_relaxed);
        verdict(tally,
                line->unmask() == Status::Ok &&
                    UNMASKS.load(std::memory_order_relaxed) == unmasks + 1,
                "and the line is no longer held once unmarked");

        // A removal that goes through leaves no hold for the next owner.
        auto const woke_again = line->handle_irq(irq::Eoi::Skip);
        auto const marked_again = line->mark_dying();
        if (marked_again != Status::Ok) {
            verdict(tally, false, "the held line is marked");
            auto _ = line->detach(owner.action); // Clean-up; the check above is the result.
            delete line;
            return;
        }
        line->revoke();
        line->unmark_dying();
        Owner next(irq::IrqFlags::Oneshot, &owner_cancel);
        auto const taken = line->attach(next.action);
        unmasks = UNMASKS.load(std::memory_order_relaxed);
        verdict(tally,
                woke_again == irq::IrqReturn::WakeThread && taken == Status::Ok &&
                    line->unmask() == Status::Ok &&
                    UNMASKS.load(std::memory_order_relaxed) == unmasks + 1,
                "a revoked line that was held is not held by the next owner");
        auto _ = line->detach(next.action); // Clean-up; the check above is the result.
        delete line;
    }

    struct Dispatch {
        irq::Line *line;
    };

    auto dispatch_routine(void *const arg) -> i32 {
        auto const result = static_cast<Dispatch *>(arg)->line->handle_irq(irq::Eoi::Skip);
        if (result == irq::IrqReturn::Handled) {
            return 0;
        }
        return 1;
    }

    /// The command thread runs on |main_cpu| for this case. The handler holds
    /// its CPU with interrupts off until the revoke is announced, so a command
    /// thread sharing that CPU could not announce it.
    auto check_barrier(Tally &tally, CpuNum const dispatch_cpu) -> void {
        auto *const line = own_line();
        if (line == nullptr) {
            verdict(tally, false, "make the line");
            return;
        }
        Hold hold;
        Owner owner(irq::IrqFlags::None, &owner_cancel);
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
        verdict(tally, attached && marked == Status::Ok && in_flight,
                "a handler of the line is in flight on another CPU");
        if (!attached || marked != Status::Ok || !in_flight) {
            hold.revoking.store(true, std::memory_order_release);
            if (join_within(thread)) {
                auto _ = line->detach(owner.action); // Clean-up; the check above is the result.
                delete line;
            }
            return;
        }

        auto const returned_before = hold.returned.load(std::memory_order_acquire);
        hold.revoking.store(true, std::memory_order_release);
        line->revoke();
        auto const returned_at_revoke = hold.returned.load(std::memory_order_acquire);

        verdict(tally, !returned_before && returned_at_revoke,
                "revoke() returns only after the handler has, which was still running");
        verdict(tally, owner.handler_returned_in_hook.load() && owner.told.load() == 1,
                "and the hook is told after it");
        if (join_within(thread)) {
            delete line;
        } else {
            verdict(tally, false, "the dispatching thread ends");
        }
    }

    /// One round at a time: main builds the owner and says go, the remover
    /// revokes through the table, the owner detaches through it after a delay
    /// of its own. The table is the test's own, made once per run and kept:
    /// a table cannot be freed.
    struct Race {
        irq::Domain *table = nullptr;
        VIrqNum virq = irq::INVALID_V_IRQ_NUM;
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
            auto _ = RACE.table->revoke_line(RACE.virq); // The owner may have gone first.
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
            auto const status = RACE.table->detach(RACE.owner->action);
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
        auto *const table = new (mm::GAF_KERNEL) irq::Domain();
        constexpr std::array<HIrqNum, 1> NUMBERS{OWN_HWIRQ};
        if (table == nullptr ||
            table->init(ustl::views::Span<HIrqNum const>(NUMBERS), CHIP) != Status::Ok) {
            verdict(tally, false, "make the race's line table");
            return;
        }
        RACE.table = table;
        RACE.virq = table->virq_of(OWN_HWIRQ);
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
            auto *const owner = new (mm::GAF_KERNEL) Owner(irq::IrqFlags::None, &owner_cancel);
            if (owner == nullptr) {
                ran = false;
                break;
            }
            owner->hook_spins = RACE_HOOK_SPINS;
            if (table->attach(RACE.virq, owner->action) != Status::Ok) {
                ran = false;
                delete owner;
                break;
            }
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
                // A thread is stuck: leave the owner to it.
                ran = false;
                break;
            }
            table->unmark_dying(RACE.virq);
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
        verdict(tally, RACE.remover_won.load() > 0,
                "and the detach did land after the revoke took the action");
    }

    auto irq_revoke_test_command(int, char **) -> int {
        Tally tally;

        check_tells_every_action(tally);
        check_dying_refuses(tally);
        check_refuses_permanent(tally);
        check_refuses_hookless(tally);
        check_held_line(tally);

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

            // On the first CPU for the barrier, away from the handler's.
            auto *const self = Thread::Current::get();
            self->set_cpu_affinity_hard(CpuMask::of(first));
            auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::No);
            if (Cpu::id() != first) {
                log::error("irqrevoke.test: FAILED -- the command did not move to CPU {}", first);
                self->set_cpu_affinity_hard(cpu_possible_mask());
                return 1;
            }
            check_barrier(tally, second);
            self->set_cpu_affinity_hard(cpu_possible_mask());

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
