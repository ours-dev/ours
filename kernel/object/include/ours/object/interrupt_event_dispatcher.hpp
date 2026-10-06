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
/// number the irq layer gives the line: the GSI on a PC.
///
/// The objects:
///   - m_line, the line, fixed at creation, after any RemapIrq.
///   - m_action, the object's place on the line: the irq layer calls its
///     handler, in interrupt context and under the line's lock (L1), with the
///     object as the context, and the handler hands the interrupt to
///     handle_interrupt(). The action is the object's own, so attaching it
///     allocates nothing and detaching it names exactly it.
///
/// The line is the object's alone: the action does not share, so a line
/// another action is on -- a kernel driver's, or another interrupt
/// object's -- refuses it with AlreadyBound, as a line the kernel holds for
/// good does (Permanent: the timer's).
///
/// A level-triggered line is masked from the moment an interrupt is
/// delivered until it is acked (MaskPostwait and UnmaskPrewait): the device
/// holds the level until its driver has dealt with it, and the end of
/// interrupt goes out as soon as the handler returns, so an unmasked line
/// would fire again at once, for as long as the driver takes. An edge line
/// is not masked: interrupts arriving before the ack fold into the pending
/// one, so the object never needs more than its one packet.
///
/// The last handle going destroys the object (on_zero_handles()), and
/// destroying it masks the line and takes the action off it: a driver that
/// dies hands its line back.
class InterruptEventDispatcher final: public InterruptDispatcher {
    using Base = InterruptDispatcher;
    using Self = InterruptEventDispatcher;

  public:
    /// Makes an interrupt object on |line|, which the caller has been
    /// granted through an Irq resource, and hangs it on the line. |options|
    /// may hold RemapIrq and one mode, nothing else (InvArgs, as for a mode
    /// that names none).
    ///
    /// In order: RemapIrq turns an ISA number into its line before anything
    /// is made; the object's action goes on the line in the same step that
    /// sets how the line signals, with a mode other than ModeDefault, or
    /// reads how it signals now, with ModeDefault, so the object masks its
    /// line until the ack exactly when the line it holds is level-triggered;
    /// the line is unmasked. |out| gets the object only once all of that has
    /// succeeded, and only from then on does losing it destroy the object.
    /// Before that a failure drops the object without destroy(), which masks
    /// the line by number: on a line another owner holds, that would silence
    /// the owner's interrupts.
    ///
    /// AlreadyBound when the line has an owner; NoResource when no line has
    /// that number, and InvArgs for the one number no line can have, as
    /// irq::attach() answers; NotSupported when ModeDefault asks a controller
    /// that cannot say how its line signals; NoMemory.
    [[nodiscard]]
    static auto create(KernelHandle<InterruptDispatcher> *out, Rights *rights, u32 line,
                       InterruptOptions options) -> Status;

  private:
    /// Edge until create() learns otherwise: whether the line is
    /// level-triggered is settled only as the action goes on it.
    explicit InterruptEventDispatcher(VIrqNum line);

    /// The action's handler: |context| is the object.
    static auto on_interrupt(VIrqNum line, void *context) -> irq::IrqReturn;

    auto mask_interrupt() -> void override;
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
