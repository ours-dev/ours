/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `pinlines.test`: every IO-APIC pin has an interrupt line
// (.agents/tasks/interrupt-system/PLAN.md, step I2). dbg image only: it is the
// one with a kernel shell.
//
// The platform programs every pin of every IO-APIC the MADT lists, masked,
// and builds a line for each, so that a device wired to a pin no ISA line
// reaches -- a PCI function's INTx -- can be taken with interrupt_create. The
// checks:
//   - the machine has pins past the ISA range, or the rest would pass with
//     nothing to look at;
//   - every pin whose GSI is within the platform vectors has a line, and one
//     past them is left without;
//   - a pin from the ISA range up is idle: its entry is masked, and it
//     signals as PCI does, level-triggered;
//   - a number past the last pin and a number past the platform vectors name
//     no line, and the invalid number is refused;
//   - an interrupt object takes the last pin with a level-low mode: the line
//     then reads level and is unmasked, a second create answers
//     AlreadyBound, a destroy lets the line go masked, and a create takes it
//     again.
//
// Nothing raises the pin. What an object does with an interrupt that arrives
// is interruptline.test's, on ISA 3; what this shows is that a pin the
// platform used to leave alone is now a line like that one.
//
// A console command rather than a unit test: the pins are the machine's own,
// read from its IO-APICs.
#include <ours/arch/apic.hpp>
#include <ours/interrupt.hpp>
#include <ours/irq/mod.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/interrupt_dispatcher.hpp>
#include <ours/object/interrupt_event_dispatcher.hpp>

#include <arch/x86/interrupt.hpp>
#include <gktl/command.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    using object::InterruptDispatcher;
    using object::InterruptEventDispatcher;
    using object::KernelHandle;

    /// The last number a line has: the platform gives a pin the vector
    /// PlatformIrqMin + its GSI, and the vectors end at PlatformIrqMax.
    constexpr auto LAST_LINE =
        usize(arch::IrqVec::PlatformIrqMax) - usize(arch::IrqVec::PlatformIrqMin);

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("pinlines.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("pinlines.test: FAIL  {}", name);
    }

    /// Whether |got| is |want|, logging both when it is not.
    auto answered(Tally &tally, Status const got, Status const want, char const *const name)
        -> void {
        if (got != want) {
            log::error("pinlines.test:   {} answered {}, expected {}", name, to_string(got),
                       to_string(want));
        }
        verdict(tally, got == want, name);
    }

    /// What the irq layer answers when asked how line |line| signals: Ok if
    /// there is such a line, NoResource if there is none, InvArgs for the
    /// invalid number.
    auto line_status(VIrqNum const line) -> Status {
        auto const mode = irq::trigger_mode(line);
        if (mode.is_ok()) {
            return Status::Ok;
        }
        return mode.unwrap_err();
    }

    /// Calls |visit| with the GSI of every pin of every IO-APIC.
    auto for_each_pin(auto const &visit) -> void {
        for (auto const &ioapic: registered_io_apics()) {
            for (auto pin = usize{0}; pin < ioapic.irq_count(); ++pin) {
                visit(static_cast<VIrqNum>(ioapic.gsi_base() + pin));
            }
        }
    }

    /// The highest GSI of any pin.
    auto last_pin() -> VIrqNum {
        auto last = VIrqNum{0};
        for_each_pin([&last](VIrqNum const gsi) -> void {
            if (gsi > last) {
                last = gsi;
            }
        });
        return last;
    }

    auto check_pins_have_lines(Tally &tally) -> void {
        auto pins = usize{0};
        auto with_line = usize{0};
        auto past_vectors = usize{0};
        for_each_pin([&](VIrqNum const gsi) -> void {
            pins += 1;
            if (gsi > LAST_LINE) {
                past_vectors += 1;
                return;
            }
            if (line_status(gsi) == Status::Ok) {
                with_line += 1;
                return;
            }
            log::error("pinlines.test:   GSI {} has no line", gsi);
        });
        log::info("pinlines.test:   {} pins, {} with a line, {} past the platform vectors", pins,
                  with_line, past_vectors);

        verdict(tally, pins > arch::NUM_ISA_IRQS, "the machine has pins past the ISA range");
        verdict(tally, with_line + past_vectors == pins,
                "every pin within the platform vectors has a line");
    }

    auto check_pins_are_idle(Tally &tally) -> void {
        auto checked = usize{0};
        auto not_masked = usize{0};
        auto not_level = usize{0};
        for_each_pin([&](VIrqNum const gsi) -> void {
            if (gsi < arch::NUM_ISA_IRQS || gsi > LAST_LINE) {
                return;
            }
            checked += 1;
            auto const masked = irq::is_masked_for_test(gsi);
            if (!masked.is_ok() || !*masked) {
                not_masked += 1;
                log::error("pinlines.test:   GSI {} is not masked", gsi);
            }
            auto const mode = irq::trigger_mode(gsi);
            if (!mode.is_ok() || *mode != irq::TriggerMode::Level) {
                not_level += 1;
                log::error("pinlines.test:   GSI {} does not signal level", gsi);
            }
        });
        log::info("pinlines.test:   {} pins from the ISA range up", checked);

        verdict(tally, checked != 0, "there are pins from the ISA range up to look at");
        verdict(tally, not_masked == 0, "each of them is masked");
        verdict(tally, not_level == 0, "and signals level-triggered, as PCI does");
    }

    auto check_numbers_without_lines(Tally &tally) -> void {
        auto const beyond = last_pin() + 1;
        // A number a pin of no IO-APIC owns: left out where it would be one
        // of the platform's own, as it is not on a machine whose last pin is
        // the last vector.
        if (beyond <= LAST_LINE) {
            answered(tally, line_status(beyond), Status::NoResource,
                     "a number past the last pin names no line");
        }
        answered(tally, line_status(static_cast<VIrqNum>(LAST_LINE + 1)), Status::NoResource,
                 "a number past the platform vectors names no line");
        answered(tally, line_status(irq::INVALID_V_IRQ_NUM), Status::InvArgs,
                 "the invalid number is refused");
    }

    auto check_object_on_a_pin(Tally &tally) -> void {
        auto const pin = last_pin();
        if (pin < arch::NUM_ISA_IRQS || pin > LAST_LINE) {
            verdict(tally, false, "there is a pin past the ISA range to take");
            return;
        }

        KernelHandle<InterruptDispatcher> first;
        KernelHandle<InterruptDispatcher> second;
        auto rights = Rights::None;
        auto const created =
            InterruptEventDispatcher::create(&first, &rights, pin, InterruptOptions::ModeLevelLow);
        answered(tally, created, Status::Ok, "an object takes the last pin, with a level-low mode");
        if (created != Status::Ok) {
            return;
        }

        auto const again =
            InterruptEventDispatcher::create(&second, &rights, pin, InterruptOptions::ModeLevelLow);
        answered(tally, again, Status::AlreadyBound, "a second object on it answers AlreadyBound");

        auto const mode = irq::trigger_mode(pin);
        verdict(tally, mode.is_ok() && *mode == irq::TriggerMode::Level,
                "the pin signals level-triggered while the object has it");
        auto const held = irq::is_masked_for_test(pin);
        verdict(tally, held.is_ok() && !*held, "and is unmasked, as a level object's line is");

        auto _ = first.dispatcher()->destroy();
        auto const let_go = irq::is_masked_for_test(pin);
        verdict(tally, let_go.is_ok() && *let_go, "a destroy lets the pin go masked");

        KernelHandle<InterruptDispatcher> retaken;
        auto const taken_again = InterruptEventDispatcher::create(&retaken, &rights, pin,
                                                                  InterruptOptions::ModeLevelLow);
        answered(tally, taken_again, Status::Ok, "and an object takes the pin again");
        if (taken_again == Status::Ok) {
            auto _ = retaken.dispatcher()->destroy();
        }
    }

    auto pin_lines_test_command(int, char **) -> int {
        Tally tally;

        check_pins_have_lines(tally);
        check_pins_are_idle(tally);
        check_numbers_without_lines(tally);
        check_object_on_a_pin(tally);

        if (tally.failed != 0) {
            log::error("pinlines.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("pinlines.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(PinLinesTest, "pinlines.test", ours::pin_lines_test_command,
             "check that every IO-APIC pin has a line, and that an object takes one the ISA "
             "lines do not reach");
