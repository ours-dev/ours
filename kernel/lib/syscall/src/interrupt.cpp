/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/irq/mod.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/interrupt_dispatcher.hpp>
#include <ours/object/interrupt_event_dispatcher.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/resource_dispatcher.hpp>
#include <ours/object/virtual_interrupt_dispatcher.hpp>
#include <ours/platform/isa.hpp>
#include <ours/status.hpp>

namespace ours::syscall {
using object::InterruptDispatcher;
using object::InterruptEventDispatcher;
using object::KernelHandle;
using object::PortDispatcher;
using object::ProcessDispatcher;
using object::VirtualInterruptDispatcher;

namespace {
    /// The options interrupt_create knows. Anything else is refused before
    /// the kind is looked at; each kind then refuses what it does not take.
    constexpr auto KNOWN_INTERRUPT_OPTIONS =
        InterruptOptions::RemapIrq | InterruptOptions::ModeMask | InterruptOptions::Virtual;
} // namespace

/// A new interrupt object. One on a physical line needs an IRQ resource that
/// grants [line, line + 1), checked before anything is made, on |line| as the
/// caller gave it; a virtual one needs none, so |resource| and |line| are not
/// looked at.
auto interrupt_create(ours::Uandle const resource, u32 const line, InterruptOptions const options,
                      user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    if (!!(options & ~KNOWN_INTERRUPT_OPTIONS)) {
        return Status::InvArgs;
    }

    KernelHandle<InterruptDispatcher> interrupt;
    Rights rights = Rights::None;
    if (!!(options & InterruptOptions::Virtual)) {
        if (auto const status = VirtualInterruptDispatcher::create(&interrupt, &rights, options);
            status != Status::Ok) {
            return status;
        }
    } else {
        if (auto const status = object::validate_ranged_resource(resource, RsrcKind::Irq, line, 1);
            status != Status::Ok) {
            return status;
        }
        if (auto const status =
                InterruptEventDispatcher::create(&interrupt, &rights, line, options);
            status != Status::Ok) {
            return status;
        }
    }
    auto handle =
        object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(interrupt)), rights);
    if (!handle) {
        return Status::NoResource;
    }

    // The value goes out before the handle goes in, as port_create does: a
    // fault here leaves no handle behind, and the object goes with it.
    auto &table = ProcessDispatcher::current()->handle_table();
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

/// The number interrupt_create takes for a source the firmware names. Nothing
/// is taken, only looked up, so it changes no state and has nothing to give
/// back; the only thing it can fail after is writing |out|.
///
/// The kind and the source are checked before the resource, as
/// interrupt_create checks its options before it looks at the resource: each
/// check names the argument it is about. The resource must be the root of the
/// interrupt lines, not a range of them: the firmware's numbers are not the
/// numbers a range is made of, so a narrowed resource cannot say which of
/// them it covers.
auto interrupt_map(ours::Uandle const resource, InterruptSourceKind const kind, u32 const source,
                   user_copy::UserOutPtr<u32> const out) -> Status {
    if (kind != InterruptSourceKind::Isa && kind != InterruptSourceKind::Gsi) {
        return Status::InvArgs;
    }
    if (source == irq::INVALID_H_IRQ_NUM) {
        return Status::InvArgs;
    }
    if (auto const status = object::validate_root_resource(resource, RsrcKind::Irq);
        status != Status::Ok) {
        return status;
    }

    auto virq = irq::INVALID_V_IRQ_NUM;
    switch (kind) {
        case InterruptSourceKind::Isa:
            virq = platform::isa_irq_line(source);
            break;
        case InterruptSourceKind::Gsi:
            virq = irq::virq_of_hwirq(source);
            break;
    }
    if (virq == irq::INVALID_V_IRQ_NUM) {
        return Status::NoResource;
    }
    return out.copy_to_user(virq);
}

/// Takes the next interrupt, blocking until one arrives: Cancelled once the
/// interrupt is destroyed, BadState while another thread waits on it. The
/// interrupt is taken before the time is written out, so a |out_timestamp|
/// that faults loses it.
auto interrupt_wait(ours::Uandle const handle, user_copy::UserOutPtr<Instant> const out_timestamp)
    -> Status {
    ustl::Arc<InterruptDispatcher> interrupt;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::Wait, &interrupt);
        status != Status::Ok) {
        return status;
    }

    Instant timestamp = 0;
    if (auto const status = interrupt->wait(&timestamp); status != Status::Ok) {
        return status;
    }
    if (out_timestamp.is_null()) {
        return Status::Ok;
    }
    return out_timestamp.copy_to_user(timestamp);
}

/// Raises a virtual interrupt by hand, as having arrived at |timestamp|. The
/// options are checked before the handle.
auto interrupt_trigger(ours::Uandle const handle, InterruptTriggerOptions const options,
                       Instant const timestamp) -> Status {
    if (options != InterruptTriggerOptions::None) {
        return Status::InvArgs;
    }

    ustl::Arc<InterruptDispatcher> interrupt;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::Signal, &interrupt);
        status != Status::Ok) {
        return status;
    }
    return interrupt->trigger(timestamp);
}

/// Ends the interrupt. No right is asked: any holder of a handle may end it,
/// as closing the last handle would.
auto interrupt_destroy(ours::Uandle const handle) -> Status {
    ustl::Arc<InterruptDispatcher> interrupt;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::None, &interrupt);
        status != Status::Ok) {
        return status;
    }
    return interrupt->destroy();
}

/// The options first, then the interrupt, then the port: each check names
/// the argument it is about. Read on the interrupt, since binding decides
/// where its interrupts are read from; Write on the port, which is queued
/// to.
auto interrupt_bind(ours::Uandle const handle, ours::Uandle const port_handle, u64 const key,
                    InterruptBindOptions const options) -> Status {
    if (options != InterruptBindOptions::Bind && options != InterruptBindOptions::Unbind) {
        return Status::InvArgs;
    }

    auto &table = ProcessDispatcher::current()->handle_table();
    ustl::Arc<InterruptDispatcher> interrupt;
    if (auto const status = table.get_dispatcher(handle, Rights::Read, &interrupt);
        status != Status::Ok) {
        return status;
    }
    ustl::Arc<PortDispatcher> port;
    if (auto const status = table.get_dispatcher(port_handle, Rights::Write, &port);
        status != Status::Ok) {
        return status;
    }
    // A port made without BindToInterrupt has no interrupt queue to serve:
    // it is the wrong kind of port for this call.
    if (!port->can_bind_to_interrupt()) {
        return Status::BadType;
    }

    if (options == InterruptBindOptions::Bind) {
        return interrupt->bind(ustl::move(port), key);
    }
    return interrupt->unbind(port);
}

/// Write, as the ack changes what the interrupt sends next.
auto interrupt_ack(ours::Uandle const handle) -> Status {
    ustl::Arc<InterruptDispatcher> interrupt;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::Write, &interrupt);
        status != Status::Ok) {
        return status;
    }
    return interrupt->ack();
}
} // namespace ours::syscall
