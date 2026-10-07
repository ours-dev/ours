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
    , m_action(&Self::on_interrupt, this, irq::IrqFlags::Oneshot, "interrupt") {}

/// The order is what keeps a failure from touching a line that is not the
/// object's. Nothing is made or programmed until the options are known to be
/// good and the number is the line's; the action goes on before the
/// unmask, so the first interrupt finds it; and the handle comes last.
///
/// The mode and the action go on the line in one step, irq::attach() with
/// the mode: under one hold of the line's lock it checks that the line is
/// free, sets the mode asked for or reads the one the line has, chooses the
/// line's flow from it, links the action, and leaves the line masked. The
/// flow is what masks a level line until the ack: the action says Oneshot,
/// which the level flow honours and the edge flow ignores. So the object
/// needs nothing from the mode attach answers; the line is held masked
/// exactly when it is level-triggered, for as long as the object is on it.
/// QEMU's q35 has ISA lines the firmware made level, 9 for one, which a
/// create that leaves the mode finds level.
///
/// As two steps, irq::configure() or irq::trigger_mode() and then
/// irq::attach(), nothing held the line between them: two creates on one
/// free line at once could each set or read its mode before either was on
/// it, and the one that took the line then held it in the mode the other
/// left, with that mode's flow: a level line it asked for left unmasked
/// after each interrupt, or an edge line it asked for held masked until
/// each ack.
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
    if (auto const trigger = irq::attach(virqnum, self->m_action, mode); trigger.is_err()) {
        return trigger.unwrap_err();
    }
    // The one unmask the object makes itself, to start the line. irq::unmask()
    // does not ask whether the line is held, so every later unmask is a
    // finalize (unmask_interrupt()), made only once the object owes nothing.
    if (auto const status = irq::unmask(virqnum); status != Status::Ok) {
        // A line attach() just took always has its controller's unmask; this
        // keeps the object from being freed with its action on the line
        // should one ever lack it.
        auto _ = irq::detach(self->m_action);
        return status;
    }

    *out = KernelHandle<InterruptDispatcher>(ustl::move(self));
    *rights = default_right();
    return Status::Ok;
}

/// WakeThread, always. The line is exclusive, so the interrupt was this
/// object's device's, and the object has handed it to the thread that
/// services the device -- a waiter, or a port's reader -- or folded it into
/// one handed over already. Either way an ack is owed for it, and on a level
/// line the flow holds the line masked until then.
///
/// Once the object is destroyed too, though it hands nothing over: the
/// device may still assert a level line, and nobody will service it.
/// Handled would have the flow unmask the line at once, and the pin would
/// raise the same interrupt again and again until destroy()'s detach;
/// WakeThread leaves the line held masked until that detach, which clears
/// held and leaves the line masked as it lets it go.
auto InterruptEventDispatcher::on_interrupt(VIrqNum, void *const context) -> irq::IrqReturn {
    static_cast<Self *>(context)->handle_interrupt();
    return irq::IrqReturn::WakeThread;
}

/// The finalize takes the line's lock, which is why the state machine calls
/// this with its own lock dropped. It names the line by m_line rather than
/// asking the action, and does nothing once destroy()'s detach has taken the
/// action off: the detach cleared held and masked the line for it, and a
/// late finalize must not unmask a line the next owner has not started.
auto InterruptEventDispatcher::unmask_interrupt() -> void {
    irq::finalize_oneshot(m_line, m_action);
}

/// The barrier: irq::detach() returns once no handler of the line runs on
/// any CPU, so after it nothing reaches this object through the line. Only a
/// created object is destroyed, and create() attached it, so the action
/// is on the line.
auto InterruptEventDispatcher::unregister_interrupt_handler() -> void {
    [[maybe_unused]]
    auto const status = irq::detach(m_action);
    DEBUG_ASSERT(Status::Ok == status, "detaching from line {} answered {}", m_line,
                 to_string(status));
}
} // namespace ours::object
