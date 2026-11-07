/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// PinLines.PinsHaveLinesNamedByVirq: every IO-APIC pin has an interrupt line,
// and a line is named by its virq, not by the pin's GSI
// (.agents/tasks/interrupt-system/PLAN.md, steps I2 and I3). dbg image only:
// it is the one with a kernel shell.
//
// The platform programs every pin of every IO-APIC the MADT lists, masked,
// and builds a line for each, so that a device wired to a pin no ISA line
// reaches -- a PCI function's INTx -- can be taken with interrupt_create. The
// irq layer's domain gives each line a virq, which is what the line is named
// by from then on. The checks:
//   - the machine has pins past the ISA range, or the rest would pass with
//     nothing to look at;
//   - every pin whose GSI is within the platform vectors has a line, and one
//     past them is left without;
//   - the domain maps each of those GSIs to a virq and back, to a virq that
//     is not the GSI, consecutive in the order of the GSIs from past the
//     highest, wherever the IO-APICs list them; and it answers INVALID for
//     what it does not hold, at both ends of both tables, the interrupt path
//     included;
//   - each ISA IRQ names the line made for the pin the MADT sends it to,
//     asked of the firmware's tables and not of the irq layer, and the
//     cascade names none (the timer code asks the same function);
//   - a pin from the ISA range up is idle: its entry is masked, and it
//     signals as PCI does, level-triggered;
//   - a GSI used as a virq names no line, and neither does the number past
//     the last virq, nor one past the platform vectors; the invalid number
//     is refused;
//   - an interrupt object takes GSI 16, the first pin past the ISA range, by
//     its virq, with a level-low mode: the line then reads level and is unmasked, a second create
//     answers AlreadyBound, a destroy lets the line go masked, and a create
//     takes it again.
//
// Nothing raises the pin. What an object does with an interrupt that arrives
// is interruptline.test's, on ISA 3; what this shows is that a pin the
// platform used to leave alone is now a line like that one.
//
// Why an in-kernel test: the pins are the machine's own, read from its
// IO-APICs.
#include <ours/arch/apic.hpp>
#include <ours/interrupt.hpp>
#include <ours/irq/domain.hpp>
#include <ours/irq/mod.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/interrupt_dispatcher.hpp>
#include <ours/object/interrupt_event_dispatcher.hpp>
#include <ours/platform/isa.hpp>

#include <algorithm>
#include <type_traits>

#include <arch/x86/interrupt.hpp>
#include <ktest/ktest.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    static_assert(!std::is_invocable_v<decltype(&irq::mask), HIrqNum>);
    static_assert(!std::is_invocable_v<decltype(&irq::DomainEndpoint::dispatch),
                                       irq::DomainEndpoint const &, VIrqNum>);
    static_assert(
        !std::is_invocable_v<decltype(&arch::IoApic::issue_eoi), arch::IoApic &, HIrqNum>);

    using object::InterruptDispatcher;
    using object::InterruptEventDispatcher;
    using object::KernelHandle;

    /// The last GSI that has a line: the platform gives a pin the vector
    /// PlatformIrqMin + its GSI, and the vectors end at PlatformIrqMax.
    constexpr auto LAST_GSI =
        usize(arch::IrqVec::PlatformIrqMax) - usize(arch::IrqVec::PlatformIrqMin);

    /// Whether |got| is |want|, logging both when it is not.
    auto answered(Status const got, Status const want, char const *const name) -> void {
        if (got != want) {
            log::error("pinlines:   {} answered {}, expected {}", name, to_string(got),
                       to_string(want));
        }
        EXPECT_EQ(want, got, "{}", name);
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

    /// Calls |visit| with the GSI of every pin of every IO-APIC, the pins of
    /// one in the order of their GSIs.
    auto for_each_pin(auto const &visit) -> void {
        for (auto const &ioapic: registered_io_apics()) {
            for (auto pin = usize{0}; pin < ioapic.irq_count(); ++pin) {
                visit(static_cast<HIrqNum>(ioapic.gsi_base() + pin));
            }
        }
    }

    auto check_pins_have_lines() -> void {
        auto pins = usize{0};
        auto with_line = usize{0};
        auto past_vectors = usize{0};
        for_each_pin([&](HIrqNum const gsi) -> void {
            pins += 1;
            if (static_cast<usize>(gsi) > LAST_GSI) {
                past_vectors += 1;
                return;
            }
            if (line_status(irq::irq_for_source(irq::DomainId::Root, gsi)) == Status::Ok) {
                with_line += 1;
                return;
            }
            log::error("pinlines:   GSI {} has no line", static_cast<u32>(gsi));
        });
        log::info("pinlines:   {} pins, {} with a line, {} past the platform vectors", pins,
                  with_line, past_vectors);

        EXPECT_GT(pins, arch::NUM_ISA_IRQS, "the machine has pins past the ISA range");
        EXPECT_EQ(with_line + past_vectors, pins,
                  "every pin within the platform vectors has a line");
    }

    auto check_domain() -> void {
        auto checked = usize{0};
        auto not_inverse = usize{0};
        auto equal = usize{0};
        auto misplaced = usize{0};
        auto highest_gsi = HIrqNum{0};
        for_each_pin([&highest_gsi](HIrqNum const gsi) -> void {
            if (static_cast<usize>(gsi) <= LAST_GSI) {
                highest_gsi = std::max(highest_gsi, gsi);
            }
        });
        // The rule of domain.hpp, written out from its words: the virqs are
        // consecutive, in the order of the GSIs wherever the IO-APICs list
        // them, from the first multiple of 32 past the highest GSI.
        auto const first_id = (static_cast<u32>(highest_gsi) / 32 + 1) * 32;
        for_each_pin([&](HIrqNum const gsi) -> void {
            if (static_cast<usize>(gsi) > LAST_GSI) {
                return;
            }
            checked += 1;
            auto const virq = irq::irq_for_source(irq::DomainId::Root, gsi);
            if (static_cast<u32>(virq) == static_cast<u32>(gsi)) {
                equal += 1;
            }
            if (irq::source_for_irq(virq) != gsi) {
                not_inverse += 1;
                log::error("pinlines:   GSI {} -> virq {} -> GSI {}", static_cast<u32>(gsi),
                           static_cast<u32>(virq), static_cast<u32>(irq::source_for_irq(virq)));
            }
            auto below = u32{0};
            for_each_pin([&](HIrqNum const other) -> void {
                if (other < gsi) {
                    below += 1;
                }
            });
            if (virq != static_cast<VIrqNum>(first_id + below)) {
                misplaced += 1;
                log::error("pinlines:   GSI {} has virq {}, expected {}", static_cast<u32>(gsi),
                           static_cast<u32>(virq), first_id + below);
            }
        });
        log::info("pinlines:   {} GSIs map to virqs from {}", checked, static_cast<u32>(first_id));

        EXPECT_NE(checked, usize{0}, "there are GSIs to map");
        EXPECT_EQ(not_inverse, usize{0}, "each GSI's virq maps back to it");
        EXPECT_EQ(equal, usize{0}, "and is not the GSI");
        EXPECT_EQ(misplaced, usize{0},
                  "the virqs are consecutive in the order of the GSIs, from past the highest");

        // The ends of both tables: one past the last pin, one either side of
        // the virqs.
        auto const last_virq = static_cast<VIrqNum>(first_id + checked - 1);
        EXPECT_EQ(irq::irq_for_source(irq::DomainId::Root,
                                      static_cast<HIrqNum>(static_cast<u32>(highest_gsi) + 1)),
                  VIrqNum::Invalid, "the hardware number past the last pin has no virq");
        EXPECT_EQ(irq::source_for_irq(static_cast<VIrqNum>(static_cast<u32>(last_virq) + 1)),
                  HIrqNum::Invalid, "the number past the last virq has no hardware number");
        EXPECT_EQ(irq::source_for_irq(static_cast<VIrqNum>(first_id - 1)), HIrqNum::Invalid,
                  "the number before the first virq has none");
        EXPECT_FALSE(irq::root_source_exists_for_test(
                         static_cast<HIrqNum>(static_cast<u32>(highest_gsi) + 1)),
                     "an interrupt on the hardware number past the last pin finds no line");
        EXPECT_EQ(irq::irq_for_source(irq::DomainId::Root, HIrqNum::Invalid), VIrqNum::Invalid,
                  "the invalid hardware number has no virq");
        EXPECT_EQ(irq::source_for_irq(VIrqNum::Invalid), HIrqNum::Invalid,
                  "the invalid virq has no hardware number");
        EXPECT_EQ(irq::irq_for_source(irq::DomainId::Root, static_cast<HIrqNum>(LAST_GSI + 1)),
                  VIrqNum::Invalid, "a hardware number past the platform vectors has no virq");
    }

    /// The ISA IRQs, whose pins the MADT's overrides may move. The line each
    /// one names must be the one made for the pin the MADT sends it to,
    /// apic_io_isa_to_global(), which comes from the firmware's tables and not
    /// from the irq layer: the other tests get their line from
    /// isa_irq_line() and use whatever it answers. The cascade, ISA 2,
    /// carries no device and names none, and nor does a number past the ISA
    /// IRQs, one that wraps as 8 bits included.
    auto check_isa_lines() -> void {
        auto wrong = usize{0};
        for (auto isa = u32{0}; isa < arch::NUM_ISA_IRQS; ++isa) {
            auto const line = platform::isa_irq_line(isa);
            if (isa == ISA_IRQ_PIC2) {
                if (line != VIrqNum::Invalid) {
                    wrong += 1;
                    log::error("pinlines:   the cascade names line {}", static_cast<u32>(line));
                }
                continue;
            }
            auto const pin = apic_io_isa_to_global(static_cast<u8>(isa));
            if (irq::source_for_irq(line) != pin) {
                wrong += 1;
                log::error("pinlines:   ISA {} gave line {}, pin {}; the MADT says pin {}", isa,
                           static_cast<u32>(line), static_cast<u32>(irq::source_for_irq(line)),
                           static_cast<u32>(pin));
            }
        }
        EXPECT_EQ(wrong, usize{0},
                  "each ISA IRQ names the line made for the pin the MADT sends it to, the cascade "
                  "none");
        EXPECT_TRUE(platform::isa_irq_line(arch::NUM_ISA_IRQS) == VIrqNum::Invalid &&
                        platform::isa_irq_line(200) == VIrqNum::Invalid &&
                        platform::isa_irq_line(0x100) == VIrqNum::Invalid &&
                        platform::isa_irq_line(0x103) == VIrqNum::Invalid,
                    "a number past the ISA IRQs is no ISA IRQ, one that wraps as 8 bits included");
    }

    auto check_pins_are_idle() -> void {
        auto checked = usize{0};
        auto not_masked = usize{0};
        auto not_level = usize{0};
        for_each_pin([&](HIrqNum const gsi) -> void {
            if (static_cast<usize>(gsi) < arch::NUM_ISA_IRQS ||
                static_cast<usize>(gsi) > LAST_GSI) {
                return;
            }
            checked += 1;
            auto const virq = irq::irq_for_source(irq::DomainId::Root, gsi);
            auto const masked = irq::is_masked_for_test(virq);
            if (!masked.is_ok() || !*masked) {
                not_masked += 1;
                log::error("pinlines:   GSI {} (virq {}) is not masked", static_cast<u32>(gsi),
                           static_cast<u32>(virq));
            }
            auto const mode = irq::trigger_mode(virq);
            if (!mode.is_ok() || *mode != irq::TriggerMode::Level) {
                not_level += 1;
                log::error("pinlines:   GSI {} (virq {}) does not signal level",
                           static_cast<u32>(gsi), static_cast<u32>(virq));
            }
        });
        log::info("pinlines:   {} pins from the ISA range up", checked);

        EXPECT_NE(checked, usize{0}, "there are pins from the ISA range up to look at");
        EXPECT_EQ(not_masked, usize{0}, "each of them is masked");
        EXPECT_EQ(not_level, usize{0}, "and signals level-triggered, as PCI does");
    }

    auto check_numbers_without_lines() -> void {
        // A GSI is no virq: every virq is past every GSI, so a program that
        // takes one for the other finds no line instead of another pin's.
        auto gsi_lines = usize{0};
        auto highest_virq = VIrqNum{0};
        for_each_pin([&](HIrqNum const gsi) -> void {
            if (static_cast<usize>(gsi) > LAST_GSI) {
                return;
            }
            if (line_status(static_cast<VIrqNum>(gsi)) != Status::NoResource) {
                gsi_lines += 1;
                log::error("pinlines:   the number {} names a line", static_cast<u32>(gsi));
            }
            highest_virq = std::max(highest_virq, irq::irq_for_source(irq::DomainId::Root, gsi));
        });
        EXPECT_EQ(gsi_lines, usize{0}, "a GSI used as a virq names no line");

        answered(line_status(static_cast<VIrqNum>(static_cast<u32>(highest_virq) + 1)),
                 Status::NoResource, "the number past the last virq names no line");
        answered(line_status(static_cast<VIrqNum>(LAST_GSI + 1)), Status::NoResource,
                 "a number past the platform vectors names no line");
        answered(line_status(VIrqNum::Invalid), Status::InvArgs, "the invalid number is refused");
    }

    auto check_object_on_a_pin() -> void {
        // GSI 16, the first pin past the ISA range. On q35 a PCI function at
        // slot 0 to 24 is wired to GSI 20 to 23 (slot + pin, modulo 4, past
        // 20), so a device attached with DISK= does not hold this one.
        auto const gsi = static_cast<HIrqNum>(arch::NUM_ISA_IRQS);
        auto const pin = irq::irq_for_source(irq::DomainId::Root, gsi);
        if (static_cast<usize>(gsi) > LAST_GSI || pin == VIrqNum::Invalid) {
            KTEST_SKIP("no pin just past the ISA range to take");
        }

        KernelHandle<InterruptDispatcher> first;
        KernelHandle<InterruptDispatcher> second;
        auto rights = Rights::None;
        auto const created =
            InterruptEventDispatcher::create(&first, &rights, pin, InterruptOptions::ModeLevelLow);
        answered(created, Status::Ok,
                 "an object takes the first pin past the ISA range by its virq, level-low");
        if (created != Status::Ok) {
            return;
        }

        auto const again =
            InterruptEventDispatcher::create(&second, &rights, pin, InterruptOptions::ModeLevelLow);
        answered(again, Status::AlreadyBound, "a second object on it answers AlreadyBound");

        auto const mode = irq::trigger_mode(pin);
        EXPECT_TRUE(mode.is_ok() && *mode == irq::TriggerMode::Level,
                    "the pin signals level-triggered while the object has it");
        auto const held = irq::is_masked_for_test(pin);
        EXPECT_TRUE(held.is_ok() && !*held, "and is unmasked, as a level object's line is");

        auto _ = first.dispatcher()->destroy();
        auto const let_go = irq::is_masked_for_test(pin);
        EXPECT_TRUE(let_go.is_ok() && *let_go, "a destroy lets the pin go masked");

        KernelHandle<InterruptDispatcher> retaken;
        auto const taken_again = InterruptEventDispatcher::create(&retaken, &rights, pin,
                                                                  InterruptOptions::ModeLevelLow);
        answered(taken_again, Status::Ok, "and an object takes the pin again");
        if (taken_again == Status::Ok) {
            auto _ = retaken.dispatcher()->destroy();
        }
    }

    KTEST(PinLines, PinsHaveLinesNamedByVirq) {
        check_pins_have_lines();
        check_domain();
        check_isa_lines();
        check_pins_are_idle();
        check_numbers_without_lines();
        check_object_on_a_pin();
    }
} // namespace
} // namespace ours
