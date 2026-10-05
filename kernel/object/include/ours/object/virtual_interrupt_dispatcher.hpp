/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_VIRTUAL_INTERRUPT_DISPATCHER_HPP
#define OURS_OBJECT_VIRTUAL_INTERRUPT_DISPATCHER_HPP 1

#include <ours/interrupt.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/interrupt_dispatcher.hpp>

namespace ours::object {
/// An interrupt with no line behind it, raised only by interrupt_trigger.
/// Zircon's VirtualInterruptDispatcher
/// (kernel/object/virtual_interrupt_dispatcher.cc), whose four operations on
/// the hardware are empty too.
///
/// It is how the state machine and the system calls are tested without a
/// device, and how one program can hand another an interrupt it raises
/// itself. Zircon's also raises a signal while it is not triggered
/// (ZX_VIRTUAL_INTERRUPT_UNTRIGGERED), so that a thread can wait for the
/// interrupt to have been taken; nothing here asks for that yet.
class VirtualInterruptDispatcher final: public InterruptDispatcher {
    using Base = InterruptDispatcher;
    using Self = VirtualInterruptDispatcher;

  public:
    /// Makes a virtual interrupt, Idle. |options| must be exactly Virtual
    /// (InvArgs), as Zircon's Create() requires the bit and refuses any it
    /// does not know. It needs no resource: it reaches no hardware.
    [[nodiscard]]
    static auto create(KernelHandle<InterruptDispatcher> *out, Rights *rights,
                       InterruptOptions options) -> Status;

  private:
    VirtualInterruptDispatcher();

    auto mask_interrupt() -> void override {}
    auto unmask_interrupt() -> void override {}
    auto deactivate_interrupt() -> void override {}
    auto unregister_interrupt_handler() -> void override {}
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_VIRTUAL_INTERRUPT_DISPATCHER_HPP
