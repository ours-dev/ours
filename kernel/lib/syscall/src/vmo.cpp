/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::syscall {
// Placeholders for vmo_create and vmo_create_child, which have no entry in
// syscalls.gni yet and so no generated sys_ declaration to define. Pointers
// for the same reason as task.cpp: object::Handle has no public constructor
// and no copy, so it cannot cross a by-value signature. They are replaced,
// not filled in, when their calls land (docs/design/260924/
// userspace-filesystem.md, 7.2): a handler's signature comes from its
// generated declaration.
auto vmo_create(object::Handle *handle) -> object::Handle *;

auto vmo_create_child(object::Handle *handle) -> object::Handle *;
} // namespace ours::syscall

namespace ours {
using object::ProcessDispatcher;
using object::VmoDispatcher;

/// The handle, then the VMO: the same two steps as Zircon's sys_vmo_read().
///
/// Resolving the handle first is what puts BadHandle > BadType >
/// AccessDenied ahead of every argument error (design 4.5), and it applies
/// to a zero-length read too -- a handle without Read is refused whatever
/// the length, as Zircon's core-vmo test pins. Everything about the range,
/// the buffer and the copy is the object's (VmoDispatcher::read_user()); this
/// layer reaches nothing below kernel/object.
auto sys_vmo_read(HandleValue const vmo, user_copy::UserOutPtr<u8> const buffer, u64 const offset,
                  usize const length) -> Status {
    ustl::Arc<VmoDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            vmo, Rights::Read, &dispatcher);
        status != Status::Ok) {
        return status;
    }
    return dispatcher->read_user(buffer, offset, length);
}

/// sys_vmo_read()'s mirror: the handle with Write, then the VMO. Zircon
/// sys_vmo_write().
auto sys_vmo_write(HandleValue const vmo, user_copy::UserInPtr<u8 const> const buffer,
                   u64 const offset, usize const length) -> Status {
    ustl::Arc<VmoDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            vmo, Rights::Write, &dispatcher);
        status != Status::Ok) {
        return status;
    }
    return dispatcher->write_user(buffer, offset, length);
}
} // namespace ours
