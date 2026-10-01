/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/resource_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::syscall {
using object::Handle;
using object::KernelHandle;
using object::ProcessDispatcher;
using object::VmoDispatcher;

/// The VMO, its handle, the value out to the caller, and only then the handle
/// into the table -- channel_create's order (design 4.1), so that a failure
/// at any step leaves nothing the program could have seen. Everything about
/// the options and the size is the object's (VmoDispatcher::create_user()).
/// Zircon sys_vmo_create().
auto vmo_create(u64 const size, VmoOptions const options,
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
auto vmo_read(HandleValue const vmo, user_copy::UserOutPtr<u8> const buffer, u64 const offset,
              usize const length) -> Status {
    ustl::Arc<VmoDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            vmo, Rights::Read, &dispatcher);
        status != Status::Ok) {
        return status;
    }
    return dispatcher->read_user(buffer, offset, length);
}

/// A child of |vmo|'s VMO -- a slice or a snapshot -- with the rights of |vmo|'s
/// handle, published as vmo_create does (design 4.1).
///
/// The handle must grant Duplicate and Read, as Zircon's does: a slice is a
/// second way into the same bytes, so it takes what a duplicate takes plus the
/// right to read them. The rights are the parent handle's, unchanged: never
/// Write added, and not Zircon's extra SetProperty either (design §10) -- a
/// slice of a read-only BootFS is read-only, which GetBackingMemory relies on
/// alongside its own handle_duplicate. Zircon sys_vmo_create_child().
auto vmo_create_child(HandleValue const vmo, VmoChildOptions const options, u64 const offset,
                      u64 const size, user_copy::UserOutPtr<HandleValue> const out) -> Status {
    ustl::Arc<VmoDispatcher> parent;
    Rights rights = Rights::None;
    auto &table = ProcessDispatcher::current()->handle_table();
    if (auto const status = table.get_dispatcher_with_rights(vmo, Rights::Duplicate | Rights::Read,
                                                             &parent, &rights);
        status != Status::Ok) {
        return status;
    }

    KernelHandle<VmoDispatcher> child;
    if (auto const status = parent->create_child(options, offset, size, child);
        status != Status::Ok) {
        return status;
    }

    auto handle = Handle::make(KernelHandle<object::Dispatcher>(ustl::move(child)), rights);
    if (!handle) {
        return Status::NoResource;
    }
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    static_cast<void>(table.add(ustl::move(handle)));
    return Status::Ok;
}

/// A new handle to |vmo|'s VMO with Execute and without Write, in place of
/// |vmo| -- which this consumes whatever the answer (design D3, K19).
///
/// Zircon's sys_vmo_replace_as_executable(), step for step:
///   1. vmex is checked first and its answer kept: a System resource covering
///      the VMEX slot, or HANDLE_INVALID, which is AccessDenied here (Zircon
///      asks the job policy, and ours has no ambient Execute to allow).
///   2. |vmo| is taken out of the table. From here it is gone, whether or not
///      the call succeeds; a caller that wants to keep a copy duplicates it
///      first (handle_duplicate). BadHandle when it names nothing, and also
///      when it names something that is not a VMO -- the handle is already
///      consumed by then, and Zircon answers BAD_HANDLE there too.
///   3. vmex's answer, now.
///   4. The new handle: the old rights less Write, plus Execute. Zircon keeps
///      Write (for JITs); ours drops it (D3), so a program cannot write the
///      code it is about to run through the handle that lets it run it.
/// The value goes out before the handle goes in, as for vmo_create.
auto vmo_replace_as_executable(HandleValue const vmo, HandleValue const vmex,
                               user_copy::UserOutPtr<HandleValue> const out) -> Status {
    auto vmex_status = Status::AccessDenied;
    if (vmex != HANDLE_INVALID) {
        vmex_status =
            object::validate_ranged_resource(vmex, RsrcKind::System, RSRC_SYSTEM_VMEX_BASE, 1);
    }

    auto &table = ProcessDispatcher::current()->handle_table();
    auto original = table.remove(vmo);
    if (!original) {
        return Status::BadHandle;
    }
    if (!original->dispatcher().downcast<VmoDispatcher>()) {
        return Status::BadHandle;
    }

    if (Status::Ok != vmex_status) {
        return vmex_status;
    }

    auto const rights = (original->rights() & ~Rights::Write) | Rights::Execute;
    auto handle = Handle::make(original->dispatcher(), rights);
    if (!handle) {
        return Status::NoResource;
    }
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    static_cast<void>(table.add(ustl::move(handle)));
    return Status::Ok;
}

/// vmo_read()'s mirror: the handle with Write, then the VMO. Zircon
/// sys_vmo_write().
auto vmo_write(HandleValue const vmo, user_copy::UserInPtr<u8 const> const buffer, u64 const offset,
               usize const length) -> Status {
    ustl::Arc<VmoDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            vmo, Rights::Write, &dispatcher);
        status != Status::Ok) {
        return status;
    }
    return dispatcher->write_user(buffer, offset, length);
}
} // namespace ours::syscall
