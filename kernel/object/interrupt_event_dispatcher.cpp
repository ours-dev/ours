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
InterruptEventDispatcher::InterruptEventDispatcher(VIrqNum const line)
    : Base(Flags::None)
    , m_line(line)
    , m_observer(&Self::on_interrupt, this, irq::IrqFlags::None, "interrupt") {}

/// The order is what keeps a failure from touching a line that is not the
/// object's. Nothing is made or programmed until the options are known to be
/// good and the number is the line's; the observer goes on before the
/// unmask, so the first interrupt finds it; and the handle comes last.
///
/// The mode and the observer go on the line in one step, irq::attach() with
/// the mode: under one hold of the line's lock it checks that the line is
/// free, sets the mode asked for or reads the one the line has, links the
/// observer, and leaves the line masked. The object has to know whether the
/// line is level-triggered to mask it until the ack, so its flags come from
/// the mode that attach answers, which is the line's for as long as the
/// object is on it. QEMU's q35 has ISA lines the firmware made level, 9 for
/// one, which a create that leaves the mode finds level.
///
/// As two steps, irq::configure() or irq::trigger_mode() and then
/// irq::attach(), nothing held the line between them: two creates on one
/// free line at once could each set or read its mode before either was on
/// it, and the one that took the line then ran with the mode the other left,
/// a level line unmasked until the ack among them.
auto InterruptEventDispatcher::create(KernelHandle<InterruptDispatcher> *const out,
                                      Rights *const rights, u32 const line,
                                      InterruptOptions const options) -> Status {
    // Only what names a physical line and how it signals: Virtual is the
    // other kind's, and nothing else exists.
    if (!!(options & ~(InterruptOptions::RemapIrq | InterruptOptions::ModeMask))) {
        return Status::InvArgs;
    }

    auto mode = ustl::Option<irq::LineMode>{ustl::NONE};
    switch (options & InterruptOptions::ModeMask) {
        case InterruptOptions::ModeDefault:
            break;
        case InterruptOptions::ModeEdgeLow:
            mode = irq::LineMode{.trigger = irq::TriggerMode::Edge,
                                 .polarity = irq::Polarity::ActiveLow};
            break;
        case InterruptOptions::ModeEdgeHigh:
            mode = irq::LineMode{.trigger = irq::TriggerMode::Edge,
                                 .polarity = irq::Polarity::ActiveHigh};
            break;
        case InterruptOptions::ModeLevelLow:
            mode = irq::LineMode{.trigger = irq::TriggerMode::Level,
                                 .polarity = irq::Polarity::ActiveLow};
            break;
        case InterruptOptions::ModeLevelHigh:
            mode = irq::LineMode{.trigger = irq::TriggerMode::Level,
                                 .polarity = irq::Polarity::ActiveHigh};
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

    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(virqnum));
    if (!self) {
        return Status::NoMemory;
    }

    // AlreadyBound here is the line's owner refusing, before the mode is
    // touched: the object goes with |self|, without destroy(), and the owner
    // keeps its line, and its mode, as they were.
    auto const trigger = irq::attach(virqnum, self->m_observer, mode);
    if (trigger.is_err()) {
        return trigger.unwrap_err();
    }
    if (*trigger == irq::TriggerMode::Level) {
        self->set_level_triggered();
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
