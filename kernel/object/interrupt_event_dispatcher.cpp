/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/object/interrupt_event_dispatcher.hpp>
#include <ours/platform/isa.hpp>

#include <heap/new.hpp>
#include <ustl/option.hpp>

namespace ours::object {
namespace {
    /// How a create asks the line to signal.
    struct LineMode {
        irq::TriggerMode trigger;
        irq::Polarity polarity;
    };
} // namespace

InterruptEventDispatcher::InterruptEventDispatcher(VIrqNum const line, Flags const flags)
    : Base(flags)
    , m_line(line)
    , m_observer(&Self::on_interrupt, this, irq::IrqFlags::None, "interrupt") {}

/// The order is what keeps a failure from touching a line that is not the
/// object's. Nothing is made or programmed until the options are known to be
/// good and the number is the line's; the mode is set while nobody is on the
/// line, as irq::configure() refuses otherwise; the observer goes on before
/// the unmask, so the first interrupt finds it; and the handle comes last.
///
/// The mode is read back from the line when the caller leaves it, since the
/// object has to know whether the line is level-triggered to mask it until
/// the ack: QEMU's q35 has ISA lines the firmware made level, 9 for one.
///
/// Setting or reading the mode and taking the line are two steps, and
/// nothing holds the line between them: two creates on one free line at once
/// can each set or read its mode before either is on it, and the one that
/// takes the line may then run with the mode the other left, a level line
/// unmasked until the ack among them (docs/repairs/261005/object.md, item 1).
auto InterruptEventDispatcher::create(KernelHandle<InterruptDispatcher> *const out,
                                      Rights *const rights, u32 const line,
                                      InterruptOptions const options) -> Status {
    // Only what names a physical line and how it signals: Virtual is the
    // other kind's, and nothing else exists.
    if (!!(options & ~(InterruptOptions::RemapIrq | InterruptOptions::ModeMask))) {
        return Status::InvArgs;
    }

    auto mode = ustl::Option<LineMode>{ustl::NONE};
    switch (options & InterruptOptions::ModeMask) {
        case InterruptOptions::ModeDefault:
            break;
        case InterruptOptions::ModeEdgeLow:
            mode =
                LineMode{.trigger = irq::TriggerMode::Edge, .polarity = irq::Polarity::ActiveLow};
            break;
        case InterruptOptions::ModeEdgeHigh:
            mode =
                LineMode{.trigger = irq::TriggerMode::Edge, .polarity = irq::Polarity::ActiveHigh};
            break;
        case InterruptOptions::ModeLevelLow:
            mode =
                LineMode{.trigger = irq::TriggerMode::Level, .polarity = irq::Polarity::ActiveLow};
            break;
        case InterruptOptions::ModeLevelHigh:
            mode =
                LineMode{.trigger = irq::TriggerMode::Level, .polarity = irq::Polarity::ActiveHigh};
            break;
        default:
            return Status::InvArgs;
    }

    // Turned into the line before anything else, so that every step below,
    // and the object, use the one number: the line's, as the irq layer
    // names it.
    auto virqnum = static_cast<VIrqNum>(line);
    if (!!(options & InterruptOptions::RemapIrq)) {
        virqnum = platform::remap_isa_irq(line);
    }

    auto trigger = irq::TriggerMode::Edge;
    if (mode.is_some()) {
        trigger = mode->trigger;
    } else {
        auto const current = irq::trigger_mode(virqnum);
        if (current.is_err()) {
            return current.unwrap_err();
        }
        trigger = *current;
    }
    auto flags = Flags::None;
    if (trigger == irq::TriggerMode::Level) {
        flags |= Flags::MaskPostwait | Flags::UnmaskPrewait;
    }

    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(virqnum, flags));
    if (!self) {
        return Status::NoMemory;
    }

    if (mode.is_some()) {
        if (auto const status = irq::configure(virqnum, mode->trigger, mode->polarity);
            status != Status::Ok) {
            return status;
        }
    }
    // AlreadyBound here is the line's owner refusing: the object goes with
    // |self|, without destroy(), and the owner keeps its line as it was.
    if (auto const status = irq::attach(virqnum, self->m_observer); status != Status::Ok) {
        return status;
    }
    if (auto const status = irq::unmask(virqnum); status != Status::Ok) {
        // A line attach() just took always has its controller's unmask; this
        // keeps the object from being freed with its observer on the line
        // should one ever lack it.
        auto _ = irq::detach(self->m_observer);
        return status;
    }

    *out = KernelHandle<InterruptDispatcher>(ustl::move(self));
    *rights = default_right();
    return Status::Ok;
}

auto InterruptEventDispatcher::on_interrupt(VIrqNum, void *const context) -> irq::IrqReturn {
    static_cast<Self *>(context)->handle_interrupt();
    // The line is exclusive, so the interrupt was this object's device's,
    // whether or not the object still delivers it.
    return irq::IrqReturn::Handled;
}

/// The two may be called in interrupt context (mask_interrupt() from
/// handle_interrupt()), which irq::mask() and irq::unmask() allow: they take
/// only the controller's lock. They answer for a line that is not there, and
/// this object's line is there for as long as the object is.
auto InterruptEventDispatcher::mask_interrupt() -> void {
    [[maybe_unused]]
    auto const status = irq::mask(m_line);
    DEBUG_ASSERT(Status::Ok == status, "masking line {} answered {}", m_line, to_string(status));
}

auto InterruptEventDispatcher::unmask_interrupt() -> void {
    [[maybe_unused]]
    auto const status = irq::unmask(m_line);
    DEBUG_ASSERT(Status::Ok == status, "unmasking line {} answered {}", m_line, to_string(status));
}

/// The barrier: irq::detach() returns once no handler of the line runs on
/// any CPU, so after it nothing reaches this object through the line. Only a
/// created object is destroyed, and create() attached it, so the observer
/// is on the line.
auto InterruptEventDispatcher::unregister_interrupt_handler() -> void {
    [[maybe_unused]]
    auto const status = irq::detach(m_observer);
    DEBUG_ASSERT(Status::Ok == status, "detaching from line {} answered {}", m_line,
                 to_string(status));
}
} // namespace ours::object
