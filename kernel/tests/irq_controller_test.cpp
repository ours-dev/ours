/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// IrqController.LineOverAChipThatCannotSayItsMode: what a line does with a
// chip that cannot say how its pins signal, which is what the contract asks
// of a controller with no register for the trigger mode: it leaves
// trigger_mode() null (.agents/tasks/irq-multi-arch/PLAN.md, step 3). dbg
// image only: it is the one with a kernel shell.
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
//
// Why an in-kernel test: Desc allocates from the kernel's heap.
#include <ours/irq/domain.hpp>
#include <ours/irq/mod.hpp>
#include <ours/irq/registration.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <initializer_list>

#include <heap/new.hpp>
#include <kernel/kernel/irq/line.hpp>
#include <ktest/ktest.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>

namespace ours {
namespace {
    /// The number of the test's own line, which the line table does not
    /// reach: it only names the line to its actions.
    constexpr auto LINE = VIrqNum{0xffff'fffe};

    auto answered(Status const got, Status const want, char const *const name) -> void {
        if (got != want) {
            log::error("irqcontroller:   {} answered {}, expected {}", name, to_string(got),
                       to_string(want));
        }
        EXPECT_EQ(want, got, "{}", name);
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
        .protocol = irq::DispatchProtocol::Eoi,
        .masking = irq::Masking::Synchronous,
        .mask = mask_pin,
        .unmask = unmask_pin,
        .configure = configure_pin,
        .issue_eoi = end_interrupt,
    };

    /// The same chip, with a pin that reads as level.
    constexpr irq::ControllerOps SEEING_CHIP = {
        .protocol = irq::DispatchProtocol::Eoi,
        .masking = irq::Masking::Synchronous,
        .mask = mask_pin,
        .unmask = unmask_pin,
        .configure = configure_pin,
        .trigger_mode = level_pin,
        .issue_eoi = end_interrupt,
    };

    auto handled(VIrqNum, void *) -> irq::IrqReturn {
        return irq::IrqReturn::Handled;
    }

    /// A line of the test's own, outside the table, bound to `chip`; null if
    /// it cannot be made. The caller deletes it, with nobody on it.
    auto make_line(irq::ControllerOps const &chip) -> irq::Line * {
        auto *const line = new (mm::ALLOC_KERNEL) irq::Line(HIrqNum{7}, LINE);
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
    auto check_attach_without_a_mode() -> void {
        irq::Registration oneshot({.handle = handled, .ready = [](void *) -> bool { return true; }},
                                  nullptr, irq::RegistrationOptions::Oneshot);
        irq::Registration plain({.handle = handled}, nullptr, irq::RegistrationOptions::None);

        auto *const blind = make_line(BLIND_CHIP);
        auto *const seeing = make_line(SEEING_CHIP);
        if (blind == nullptr || seeing == nullptr) {
            delete blind;
            delete seeing;
            FAIL("make the lines of the test's own");
        }

        answered(blind->attach(oneshot), Status::NotSupported,
                 "a Oneshot action with no mode is refused by a chip that cannot say its mode");
        EXPECT_TRUE(!oneshot.is_attached(), "and it is not left on the line");
        answered(blind->attach(plain), Status::Ok,
                 "a plain action with no mode is taken by that chip's line");
        EXPECT_TRUE(blind->detach(plain) == Status::Ok && !plain.is_attached(),
                    "and gives the line back");
        // A refusal keeps nothing: the line takes a Oneshot action once it
        // is given a mode.
        auto const level = ustl::some(LEVEL_MODE);
        EXPECT_TRUE(blind->attach(oneshot, level).is_ok(),
                    "after a refusal the same line takes a Oneshot action that brings a mode");
        EXPECT_TRUE(blind->detach(oneshot) == Status::Ok, "and gives it back");

        answered(seeing->attach(oneshot), Status::Ok,
                 "a Oneshot action with no mode is taken by a chip that can say its mode");
        EXPECT_TRUE(seeing->dispatch_trigger() == irq::TriggerMode::Level,
                    "and its line runs the flow of the mode the chip says");
        EXPECT_TRUE(seeing->detach(oneshot) == Status::Ok, "and gives the line back");
        answered(seeing->attach(plain), Status::Ok,
                 "a plain action with no mode is taken by that chip's line too");
        EXPECT_TRUE(seeing->detach(plain) == Status::Ok, "and gives the line back");

        delete blind;
        delete seeing;
    }

    /// The actions of an attach with a mode.
    auto check_attach_with_a_mode() -> void {
        irq::Registration oneshot({.handle = handled, .ready = [](void *) -> bool { return true; }},
                                  nullptr, irq::RegistrationOptions::Oneshot);

        auto *const blind = make_line(BLIND_CHIP);
        if (blind == nullptr) {
            FAIL("make the line of the test's own");
        }

        CONFIGURES.store(0, std::memory_order_relaxed);
        auto const level = ustl::some(LEVEL_MODE);
        auto const taken = blind->attach(oneshot, level);
        EXPECT_TRUE(taken.is_ok() && taken.unwrap() == irq::TriggerMode::Level,
                    "a Oneshot action that brings a mode is taken by a chip that cannot say its "
                    "mode, in that mode");
        EXPECT_TRUE(CONFIGURES.load(std::memory_order_relaxed) == 1 &&
                        TOLD_TRIGGER.load(std::memory_order_relaxed) == irq::TriggerMode::Level,
                    "and the chip is told the mode, once");
        EXPECT_TRUE(blind->dispatch_trigger() == irq::TriggerMode::Level,
                    "and the line runs the flow of the mode it was given");
        EXPECT_TRUE(blind->detach(oneshot) == Status::Ok, "and gives the line back");

        auto const read = blind->attach(oneshot, ustl::none());
        answered(read.is_err() ? read.unwrap_err() : Status::Ok, Status::NotSupported,
                 "with no mode to give and none to read, the chip that cannot say refuses");
        EXPECT_TRUE(!oneshot.is_attached(), "and the action is not left on the line");

        delete blind;
    }

    enum class Phase : u8 { Ack, Mask, Handle, Unmask, End };

    struct HandshakeProbe {
        std::array<Phase, 8> events{};
        usize count = 0;
        irq::TriggerMode trigger = irq::TriggerMode::Edge;
        irq::IrqReturn answer = irq::IrqReturn::Handled;
        bool ready = true;

        auto record(Phase const phase) -> void {
            ASSERT(count < events.size());
            events[count++] = phase;
        }
        [[nodiscard]]
        auto matches(std::initializer_list<Phase> const expected) const -> bool {
            return count == expected.size() &&
                   std::equal(expected.begin(), expected.end(), events.begin());
        }
    };

    constexpr irq::ControllerOps END_OPS = {
        .protocol = irq::DispatchProtocol::Eoi,
        .masking = irq::Masking::Synchronous,
        .mask =
            [](irq::ControllerSource const &source) {
                static_cast<HandshakeProbe *>(source.context)->record(Phase::Mask);
            },
        .unmask =
            [](irq::ControllerSource const &source) {
                static_cast<HandshakeProbe *>(source.context)->record(Phase::Unmask);
            },
        .trigger_mode =
            [](irq::ControllerSource const &source) {
                return static_cast<HandshakeProbe *>(source.context)->trigger;
            },
        .issue_eoi =
            [](irq::ControllerSource const &source) {
                static_cast<HandshakeProbe *>(source.context)->record(Phase::End);
            },
    };
    constexpr auto ACK_OPS = [] {
        auto operations = END_OPS;
        operations.protocol = irq::DispatchProtocol::AckEoi;
        operations.issue_ack = [](irq::ControllerSource const &source) {
            static_cast<HandshakeProbe *>(source.context)->record(Phase::Ack);
        };
        return operations;
    }();

    auto handshake_handler(VIrqNum, void *const context) -> irq::IrqReturn {
        auto &probe = *static_cast<HandshakeProbe *>(context);
        probe.record(Phase::Handle);
        return probe.answer;
    }

    auto check_handshake(irq::ControllerOps const &operations, irq::TriggerMode const trigger)
        -> void {
        HandshakeProbe probe;
        probe.trigger = trigger;
        auto *const line = new (mm::ALLOC_KERNEL) irq::Line(HIrqNum{7}, LINE);
        if (line == nullptr) {
            FAIL("allocate the handshake line");
        }
        answered(line->init(operations, &probe), Status::Ok,
                 "bind a validated handshake and controller context");
        irq::Registration registration({.handle = handshake_handler}, &probe,
                                       irq::RegistrationOptions::None);
        answered(line->attach(registration), Status::Ok, "attach the handshake handler");
        auto const handled_result = line->handle_irq(irq::Eoi::Send);
        auto const ack = operations.protocol == irq::DispatchProtocol::AckEoi;
        auto ordered = probe.matches({Phase::Handle, Phase::End});
        if (ack) {
            ordered = probe.matches({Phase::Ack, Phase::Handle, Phase::End});
        }
        EXPECT_TRUE(handled_result == irq::IrqReturn::Handled && ordered,
                    "declared handshake order holds independently of Edge or Level");
        auto _ = line->detach(registration);
        probe.count = 0;
        auto const unclaimed = line->handle_irq(irq::Eoi::Send);
        ordered = probe.matches({Phase::Mask, Phase::End});
        if (ack) {
            ordered = probe.matches({Phase::Ack, Phase::Mask, Phase::End});
        }
        EXPECT_TRUE(unclaimed == irq::IrqReturn::None && ordered,
                    "an unclaimed source is acknowledged, masked and ended exactly once");
        probe.count = 0;
        auto const attached = line->attach(registration);
        auto const synthetic = line->handle_irq(irq::Eoi::Skip);
        EXPECT_TRUE(attached == Status::Ok && synthetic == irq::IrqReturn::Handled &&
                        probe.matches({Phase::Handle}),
                    "synthetic delivery issues neither ACK nor EOI");
        _ = line->detach(registration);
        delete line;
    }

    auto check_handshake_contract() -> void {
        check_handshake(END_OPS, irq::TriggerMode::Edge);
        check_handshake(END_OPS, irq::TriggerMode::Level);
        check_handshake(ACK_OPS, irq::TriggerMode::Edge);
        check_handshake(ACK_OPS, irq::TriggerMode::Level);

        HandshakeProbe probe;
        probe.trigger = irq::TriggerMode::Level;
        probe.answer = irq::IrqReturn::WakeThread;
        probe.ready = false;
        auto *const line = new (mm::ALLOC_KERNEL) irq::Line(HIrqNum{7}, LINE);
        if (line == nullptr) {
            FAIL("allocate the hold line");
        }
        auto malformed = ACK_OPS;
        malformed.issue_ack = nullptr;
        answered(line->init(malformed, &probe), Status::InvArgs,
                 "AckEoi refuses a missing acknowledge hook before binding");
        malformed = ACK_OPS;
        malformed.protocol = irq::DispatchProtocol::Eoi;
        answered(line->init(malformed, &probe), Status::InvArgs,
                 "Eoi refuses an unused acknowledge hook");
        malformed = END_OPS;
        malformed.protocol = irq::DispatchProtocol::Invalid;
        answered(line->init(malformed, &probe), Status::InvArgs,
                 "a controller must declare its handshake");
        malformed.protocol = static_cast<irq::DispatchProtocol>(255);
        answered(line->init(malformed, &probe), Status::InvArgs,
                 "unknown handshake declarations are refused");
        malformed = END_OPS;
        malformed.masking = static_cast<irq::Masking>(255);
        answered(line->init(malformed, &probe), Status::InvArgs,
                 "unknown masking capabilities are refused");
        answered(line->init(ACK_OPS, &probe), Status::Ok,
                 "refused contracts leave the line available for a valid binding");
        irq::Registration registration(
            {.handle = handshake_handler,
             .ready =
                 [](void *const context) { return static_cast<HandshakeProbe *>(context)->ready; }},
            &probe, irq::RegistrationOptions::Oneshot);
        answered(line->attach(registration), Status::Ok,
                 "a synchronous controller accepts deferred work");
        auto const result = line->handle_irq(irq::Eoi::Send);
        EXPECT_TRUE(result == irq::IrqReturn::WakeThread &&
                        probe.matches({Phase::Ack, Phase::Mask, Phase::Handle, Phase::End}),
                    "deferred work masks before notification and still ends on the receiving CPU");
        probe.count = 0;
        line->finalize_oneshot(registration);
        EXPECT_TRUE(probe.count == 0, "completion cannot reopen work whose owner is not ready");
        probe.ready = true;
        line->finalize_oneshot(registration);
        EXPECT_TRUE(probe.matches({Phase::Unmask}),
                    "ready completion only unmasks and never repeats ACK or EOI");
        auto _ = line->detach(registration);
        delete line;

        auto ordinary_only = END_OPS;
        ordinary_only.masking = irq::Masking::Unspecified;
        auto *const restricted = new (mm::ALLOC_KERNEL) irq::Line(HIrqNum{7}, LINE);
        if (restricted == nullptr) {
            FAIL("make the restricted-capability line");
        }
        answered(restricted->init(ordinary_only, &probe), Status::Ok,
                 "ordinary-only masking binds with explicit context");
        answered(restricted->attach(registration), Status::NotSupported,
                 "mask and unmask pointers alone cannot promise synchronous deferred holding");
        delete restricted;
    }

    KTEST(IrqController, LineOverAChipThatCannotSayItsMode) {
        check_handshake_contract();
        check_attach_without_a_mode();
        check_attach_with_a_mode();
    }
} // namespace
} // namespace ours
