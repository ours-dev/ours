/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_INTERRUPT_EVENT_DISPATCHER_HPP
#define OURS_OBJECT_INTERRUPT_EVENT_DISPATCHER_HPP 1

#include <ours/interrupt.hpp>
#include <ours/irq/action.hpp>
#include <ours/irq/types.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/interrupt_dispatcher.hpp>

namespace ours::object {
/// An interrupt object on a physical interrupt line, which it names by the
/// number the irq layer gives the line, its virq. That is not the pin's
/// hardware number (the GSI on a PC).
///
/// The objects:
///   - m_line, the line, fixed at creation.
///   - m_action, the object's place on the line: the irq layer calls its
///     handler, in interrupt context and under the line's lock (L1), with the
///     object as the context, and the handler hands the interrupt to
///     handle_interrupt(). The action is the object's own, so attaching it
///     allocates nothing and detaching it names exactly it. It says Oneshot.
///
/// The line is the object's alone: the action does not share, so a line
/// another action is on -- a kernel driver's, or another interrupt
/// object's -- refuses it with AlreadyBound, as a line the kernel holds for
/// good does (Permanent: the timer's).
///
/// A level-triggered line is masked from the moment an interrupt is
/// delivered until it is acked: the device holds the level until its driver
/// has dealt with it, and the end of interrupt goes out as soon as the
/// handler returns, so an unmasked line would fire again at once, for as long
/// as the driver takes. The line's flow does the masking, not the object:
/// the handler answers WakeThread for every interrupt, so the level flow of
/// a Oneshot line masks the line before the handler and keeps it held masked
/// until the object's ack finalizes it (unmask_interrupt(), which calls
/// irq::finalize_oneshot()). An edge line is not masked: its flow ignores
/// Oneshot, and interrupts arriving before the ack fold into the pending
/// one, so the object never needs more than its one packet.
///
/// The last handle going destroys the object (on_zero_handles()), and
/// destroying it takes the action off the line, which leaves the line
/// masked: a driver that dies hands its line back.
class InterruptEventDispatcher final: public InterruptDispatcher {
    using Base = InterruptDispatcher;
    using Self = InterruptEventDispatcher;

  public:
    /// Makes an interrupt object on |line|, which the caller has been
    /// granted through an Irq resource, and hangs it on the line. |options|
    /// may hold one mode, nothing else (InvArgs, as for a mode that names
    /// none, and for the retired bit 0x1).
    ///
    /// In order: the object's action goes on the line in the same step that
    /// sets how the line signals, with a mode other than ModeDefault, or
    /// reads how it signals now, with ModeDefault, and the line's flow
    /// follows that mode, so the line is held masked until the ack exactly
    /// when the line the object holds is level-triggered; the line is
    /// unmasked, unless an interrupt that reached the object since the
    /// attach holds it masked until its ack. |out| gets the object only once
    /// all of that has succeeded, and only from then on does losing it
    /// destroy the object.
    /// Before that a failure drops the object without destroy(), which lets
    /// go of a line the object is on: before the attach it is on none, and
    /// the detach would answer NotFound for an action no line has.
    ///
    /// AlreadyBound when the line has an owner; NoResource when no line has
    /// that number, and InvArgs for the one number no line can have, as
    /// irq::attach() answers; NotSupported when ModeDefault asks a controller
    /// that cannot say how its line signals; NoMemory.
    [[nodiscard]]
    static auto create(KernelHandle<InterruptDispatcher> *out, Rights *rights, u32 line,
                       InterruptOptions options) -> Status;

  private:
    explicit InterruptEventDispatcher(VIrqNum line);

    /// The action's handler: |context| is the object.
    static auto on_interrupt(VIrqNum line, void *context) -> irq::IrqReturn;

    /// Empty: the object writes no mask of its own. The line's flow masks a
    /// level line from each interrupt until the ack, and destroy()'s detach
    /// leaves the line masked as it lets it go, both under the line's lock,
    /// so the mask has one writer.
    auto mask_interrupt() -> void override {}
    /// Finalizes the line: it stops being held and is unmasked.
    auto unmask_interrupt() -> void override;
    /// Empty: no controller here leaves an interrupt active once its end of
    /// interrupt is sent, so there is nothing to deactivate.
    auto deactivate_interrupt() -> void override {}
    auto unregister_interrupt_handler() -> void override;

    VIrqNum const m_line;
    irq::Action m_action;
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_INTERRUPT_EVENT_DISPATCHER_HPP
