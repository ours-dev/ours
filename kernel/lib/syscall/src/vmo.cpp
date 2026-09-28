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
// Placeholder for vmo_create_child, which has no entry in syscalls.gni yet
// and so no generated sys_ declaration to define. A pointer for the same
// reason as task.cpp: object::Handle has no public constructor and no copy,
// so it cannot cross a by-value signature. It is replaced, not filled in,
// when its call lands (docs/design/260924/userspace-filesystem.md, 7.2): a
// handler's signature comes from its generated declaration.
auto vmo_create_child(object::Handle *handle) -> object::Handle *;
} // namespace ours::syscall

namespace ours {
using object::Handle;
using object::KernelHandle;
using object::ProcessDispatcher;
using object::VmoDispatcher;

/// The VMO, its handle, the value out to the caller, and only then the handle
/// into the table -- channel_create's order (design 4.1), so that a failure
/// at any step leaves nothing the program could have seen. Everything about
/// the options and the size is the object's (VmoDispatcher::create_user()).
/// Zircon sys_vmo_create().
auto sys_vmo_create(u64 const size, VmoOptions const options,
                    user_copy::UserOutPtr<HandleValue> const out) -> Status {
    KernelHandle<VmoDispatcher> vmo;
    Rights rights = Rights::None;
    if (auto const status = VmoDispatcher::create_user(size, options, vmo, rights);
        status != Status::Ok) {
        return status;
    }

    // Counts against this process's handle quota: make() is what refuses past
    // it, as it does for channel_create.
    auto handle = Handle::make(KernelHandle<object::Dispatcher>(ustl::move(vmo)), rights);
    if (!handle) {
        return Status::NoResource;
    }

    auto &table = ProcessDispatcher::current()->handle_table();
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    static_cast<void>(table.add(ustl::move(handle)));
    return Status::Ok;
}

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
