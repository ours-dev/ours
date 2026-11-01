/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_VIRTUAL_INTERRUPT_DISPATCHER_HPP
#define OURS_OBJECT_VIRTUAL_INTERRUPT_DISPATCHER_HPP 1

#include <ours/interrupt.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/interrupt_dispatcher.hpp>

namespace ours::object {
/// An interrupt with no line behind it, raised only by interrupt_trigger. Its
/// operations on the hardware are empty, but for the one signal it keeps.
///
/// It is how the state machine and the system calls are tested without a
/// device, and how one program can hand another an interrupt it raises
/// itself: a driver for a controller behind a root line (a bank of GPIO pins)
/// makes one for each pin, raises it when the pin fires, and hands its client
/// a handle without Signal.
///
/// The signal is Idle: up while the object is owed no ack, so that the
/// driver that raised it can wait for the client to have taken and acked the
/// interrupt, and then unmask the pin it masked. It is up from the creation,
/// and raised for good by a destroy. interrupt_trigger drops it before the
/// interrupt can be taken; the seam that unmasks a physical line raises it
/// (unmask_interrupt()), since it is called exactly when the object is owed
/// nothing more. A client that leaves without acking raises nothing: the
/// driver keeps the pin masked, which is the safe state, until it is handed a
/// new interrupt for it.
class VirtualInterruptDispatcher final: public InterruptDispatcher {
    using Base = InterruptDispatcher;
    using Self = VirtualInterruptDispatcher;

  public:
    /// Makes a virtual interrupt, Idle. |options| must be exactly Virtual
    /// (InvArgs): the bit is required, and any option besides it refused. It
    /// needs no resource: it reaches no hardware.
    [[nodiscard]]
    static auto create(KernelHandle<InterruptDispatcher> *out, Rights *rights,
                       InterruptOptions options) -> Status;

  private:
    VirtualInterruptDispatcher();

    auto mask_interrupt() -> void override {}
    /// The ack of the last interrupt delivered: Idle goes up.
    auto unmask_interrupt() -> void override;
    auto deactivate_interrupt() -> void override {}
    auto unregister_interrupt_handler() -> void override {}
    auto on_trigger() -> void override;
    auto on_destroy() -> void override;
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_VIRTUAL_INTERRUPT_DISPATCHER_HPP
