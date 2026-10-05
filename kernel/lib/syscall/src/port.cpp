/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/status.hpp>

namespace ours::syscall {
using object::KernelHandle;
using object::PortDispatcher;
using object::ProcessDispatcher;

/// A new port, and a handle to it. The port checks |options|.
auto port_create(PortCreateOptions const options, user_copy::UserOutPtr<ours::Uandle> const out)
    -> Status {
    KernelHandle<PortDispatcher> port;
    Rights rights = Rights::None;
    if (auto const status = PortDispatcher::create(options, &port, &rights); status != Status::Ok) {
        return status;
    }
    auto handle = object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(port)), rights);
    if (!handle) {
        return Status::NoResource;
    }

    // The value goes out before the handle goes in, as channel_create does
    // (design 4.1): a fault here leaves no handle behind.
    auto &table = ProcessDispatcher::current()->handle_table();
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

/// The handle first, then the packet: a bad handle outranks a bad pointer
/// (design 4.5).
auto port_queue(ours::Uandle const handle, user_copy::UserInPtr<PortPacket const> const packet)
    -> Status {
    ustl::Arc<PortDispatcher> port;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::Write, &port);
        status != Status::Ok) {
        return status;
    }

    PortPacket copy;
    if (auto const status = packet.copy_from_user(&copy); status != Status::Ok) {
        return status;
    }
    return port->queue_user(copy);
}

/// The packet is taken off the queue before it is written out, so a
/// |packet| that faults loses it: putting it back at the front would let a
/// caller with a bad pointer jam the port for every other waiter.
auto port_wait(ours::Uandle const handle, Instant const deadline,
               user_copy::UserOutPtr<PortPacket> const packet) -> Status {
    ustl::Arc<PortDispatcher> port;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::Read, &port);
        status != Status::Ok) {
        return status;
    }

    PortPacket taken;
    if (auto const status = port->dequeue(deadline, &taken); status != Status::Ok) {
        return status;
    }
    return packet.copy_to_user(taken);
}
} // namespace ours::syscall
