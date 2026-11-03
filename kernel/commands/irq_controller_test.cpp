/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `irqcontroller.test`: what a line does with a chip that cannot say how its pins
// signal, which is what the contract asks of a controller with no register for
// the trigger mode: it leaves trigger_mode() null
// (.agents/tasks/irq-multi-arch/PLAN.md, step 3). dbg image only: it is the
// one with a kernel shell.
//
// It touches no hardware and no x86 type. The line is the test's own, built
// outside the line table over chips that keep nothing, so it runs on any
// architecture that has the shell. Without a mode a line keeps the flow it
// took from its chip when it was bound, and a chip that cannot say gives the
// edge flow, which does not hold a Oneshot line masked, so:
//   - an action that comes with no mode: a Oneshot one is refused
//     (NotSupported) by a chip that cannot say its mode, and taken by one
//     that can; a plain one is taken by both; a refusal keeps nothing, so the
//     same line takes an action after it;
//   - an action that comes with a mode: a Oneshot one is taken by the chip
//     that cannot say, which is told the mode, and the line runs the flow of
//     that mode; with no mode to give and none to read (attach with `none`)
//     the chip that cannot say refuses it;
//   - irq::set_affinity() answers NotSupported: no controller moves a line.
//
// A console command rather than a unit test: Desc allocates from the kernel's
// heap.
#include <ours/irq/line.hpp>
#include <ours/irq/mod.hpp>
#include <ours/irq/registration.hpp>

#include <atomic>

#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>

namespace ours {
namespace {
    /// The number of the test's own line, which the line table does not
    /// reach: it only names the line to its actions.
    constexpr auto LINE = irq::INVALID_V_IRQ_NUM - 1;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("irqcontroller.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("irqcontroller.test: FAIL  {}", name);
    }

    auto answered(Tally &tally, Status const got, Status const want, char const *const name)
        -> void {
        if (got != want) {
            log::error("irqcontroller.test:   {} answered {}, expected {}", name, to_string(got),
                       to_string(want));
        }
        verdict(tally, got == want, name);
    }

    // What the chips below were told. Atomic because a chip's operations run in
    // whatever context the line's flow does; the test reads them where it
    // alone drives the line.
    constinit std::atomic<usize> CONFIGURES{0};
    constinit std::atomic<irq::TriggerMode> TOLD_TRIGGER{irq::TriggerMode::Edge};

    auto mask_pin(irq::ControllerSource const &) -> void {}

    auto unmask_pin(irq::ControllerSource const &) -> void {}

    auto end_interrupt(irq::ControllerSource const &) -> void {}

    /// Leaves the pin masked, as a chip's configure must, and notes the mode
    /// it was told.
    auto configure_pin(irq::ControllerSource const &, irq::TriggerMode const trigger, irq::Polarity)
        -> Status {
        CONFIGURES.fetch_add(1, std::memory_order_relaxed);
        TOLD_TRIGGER.store(trigger, std::memory_order_relaxed);
        return Status::Ok;
    }

    auto level_pin(irq::ControllerSource const &) -> irq::TriggerMode {
        return irq::TriggerMode::Level;
    }

    /// A chip that can mask, unmask and end an interrupt and accept a mode, but
    /// cannot say what the mode is: a controller with no mode register.
    constexpr irq::ControllerOps BLIND_CHIP = {
        .mask = mask_pin,
        .unmask = unmask_pin,
        .configure = configure_pin,
        .issue_eoi = end_interrupt,
        .name = "irqchip-test",
    };

    /// The same chip, with a pin that reads as level.
    constexpr irq::ControllerOps SEEING_CHIP = {
        .mask = mask_pin,
        .unmask = unmask_pin,
        .configure = configure_pin,
        .trigger_mode = level_pin,
        .issue_eoi = end_interrupt,
        .name = "irqchip-test",
    };

    auto handled(VIrqNum, void *) -> irq::IrqReturn {
        return irq::IrqReturn::Handled;
    }

    /// A line of the test's own, outside the table, bound to `chip`; null if
    /// it cannot be made. The caller deletes it, with nobody on it.
    auto make_line(irq::ControllerOps const &chip) -> irq::Line * {
        auto *const line = irq::Line::create(LINE, LINE, irq::IrqFlags::None, "irqchip-test");
        if (line == nullptr) {
            return nullptr;
        }
        if (line->init(chip) != Status::Ok) {
            delete line;
            return nullptr;
        }
        return line;
    }

    constexpr auto LEVEL_MODE =
        irq::LineMode{.trigger = irq::TriggerMode::Level, .polarity = irq::Polarity::ActiveHigh};

    /// The actions of an attach with no mode on the two chips.
    auto check_attach_without_a_mode(Tally &tally) -> void {
        irq::Registration oneshot({.handle = handled, .ready = [](void *) -> bool { return true; }},
                                  nullptr, irq::IrqFlags::Oneshot, "irqchip-test");
        irq::Registration plain({.handle = handled}, nullptr, irq::IrqFlags::None, "irqchip-test");

        auto *const blind = make_line(BLIND_CHIP);
        auto *const seeing = make_line(SEEING_CHIP);
        if (blind == nullptr || seeing == nullptr) {
            verdict(tally, false, "make the lines of the test's own");
            delete blind;
            delete seeing;
            return;
        }

        answered(tally, blind->attach(oneshot), Status::NotSupported,
                 "a Oneshot action with no mode is refused by a chip that cannot say its mode");
        verdict(tally, !oneshot.is_attached(), "and it is not left on the line");
        answered(tally, blind->attach(plain), Status::Ok,
                 "a plain action with no mode is taken by that chip's line");
        verdict(tally, blind->detach(plain) == Status::Ok && !plain.is_attached(),
                "and gives the line back");
        // A refusal keeps nothing: the line takes a Oneshot action once it
        // is given a mode.
        auto const level = ustl::some(LEVEL_MODE);
        verdict(tally, blind->attach(oneshot, level).is_ok(),
                "after a refusal the same line takes a Oneshot action that brings a mode");
        verdict(tally, blind->detach(oneshot) == Status::Ok, "and gives it back");

        answered(tally, seeing->attach(oneshot), Status::Ok,
                 "a Oneshot action with no mode is taken by a chip that can say its mode");
        verdict(tally, seeing->flow() == irq::Flow::FastEoi,
                "and its line runs the flow of the mode the chip says");
        verdict(tally, seeing->detach(oneshot) == Status::Ok, "and gives the line back");
        answered(tally, seeing->attach(plain), Status::Ok,
                 "a plain action with no mode is taken by that chip's line too");
        verdict(tally, seeing->detach(plain) == Status::Ok, "and gives the line back");

        delete blind;
        delete seeing;
    }

    /// The actions of an attach with a mode.
    auto check_attach_with_a_mode(Tally &tally) -> void {
        irq::Registration oneshot({.handle = handled, .ready = [](void *) -> bool { return true; }},
                                  nullptr, irq::IrqFlags::Oneshot, "irqchip-test");

        auto *const blind = make_line(BLIND_CHIP);
        if (blind == nullptr) {
            verdict(tally, false, "make the line of the test's own");
            return;
        }

        CONFIGURES.store(0, std::memory_order_relaxed);
        auto const level = ustl::some(LEVEL_MODE);
        auto const taken = blind->attach(oneshot, level);
        verdict(tally, taken.is_ok() && taken.unwrap() == irq::TriggerMode::Level,
                "a Oneshot action that brings a mode is taken by a chip that cannot say its "
                "mode, in that mode");
        verdict(tally,
                CONFIGURES.load(std::memory_order_relaxed) == 1 &&
                    TOLD_TRIGGER.load(std::memory_order_relaxed) == irq::TriggerMode::Level,
                "and the chip is told the mode, once");
        verdict(tally, blind->flow() == irq::Flow::FastEoi,
                "and the line runs the flow of the mode it was given");
        verdict(tally, blind->detach(oneshot) == Status::Ok, "and gives the line back");

        auto const read = blind->attach(oneshot, ustl::none());
        answered(tally, read.is_err() ? read.unwrap_err() : Status::Ok, Status::NotSupported,
                 "with no mode to give and none to read, the chip that cannot say refuses");
        verdict(tally, !oneshot.is_attached(), "and the action is not left on the line");

        delete blind;
    }

    auto irq_chip_test_command(int, char **) -> int {
        Tally tally;

        check_attach_without_a_mode(tally);
        check_attach_with_a_mode(tally);
        answered(tally, irq::set_affinity(LINE, CpuMask::of(CpuNum{0})), Status::NotSupported,
                 "set_affinity answers NotSupported: no controller moves a line");

        if (tally.failed != 0) {
            log::error("irqcontroller.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("irqcontroller.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(IrqChipTest, "irqcontroller.test", ours::irq_chip_test_command,
             "run a line over chips that cannot say how their pins signal, and check what it "
             "takes with and without a mode");
