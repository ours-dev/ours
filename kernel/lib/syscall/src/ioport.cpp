/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/process_dispatcher.hpp>
#include <ours/object/resource_dispatcher.hpp>
#include <ours/resource.hpp>
#include <ours/status.hpp>

namespace ours::syscall {
using object::ProcessDispatcher;
using object::validate_ranged_resource;

/// The resource is the whole check. An I/O port has no point of use the
/// kernel sees -- once open, an in or out runs on the device directly -- so
/// this is the only moment it can refuse one, and the resource, with its
/// list of the kernel's own ports, is what it refuses by.
///
/// The ports are the calling process's, not a process named by a handle: a
/// driver opens ports for itself.
auto ioports_request(ours::Uandle const resource, u16 const io_addr, u32 const len) -> Status {
    if (auto const status = validate_ranged_resource(resource, RsrcKind::IoPort, io_addr, len);
        status != Status::Ok) {
        return status;
    }
    return ProcessDispatcher::current()->open_ioports(io_addr, len);
}

/// Checked against a resource as the request is, so that the two calls
/// mirror each other, though closing a port takes nothing from anyone.
auto ioports_release(ours::Uandle const resource, u16 const io_addr, u32 const len) -> Status {
    if (auto const status = validate_ranged_resource(resource, RsrcKind::IoPort, io_addr, len);
        status != Status::Ok) {
        return status;
    }
    return ProcessDispatcher::current()->close_ioports(io_addr, len);
}
} // namespace ours::syscall
