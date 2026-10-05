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
using object::KernelHandle;
using object::ProcessDispatcher;
using object::VmoDispatcher;

/// The VMO, its handle, the value out to the caller, and only then the handle
/// into the table -- channel_create's order (design 4.1), so that a failure
/// at any step leaves nothing the program could have seen. Everything about
/// the options and the size is the object's (VmoDispatcher::create_user()).
auto vmo_create(u64 const size, VmoOptions const options,
                user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    KernelHandle<VmoDispatcher> vmo;
    Rights rights = Rights::None;
    if (auto const status = VmoDispatcher::create_user(size, options, vmo, rights);
        status != Status::Ok) {
        return status;
    }

    // Counts against this process's handle quota: make() is what refuses past
    // it, as it does for channel_create.
    auto handle = object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(vmo)), rights);
    if (!handle) {
        return Status::NoResource;
    }

    auto &table = ProcessDispatcher::current()->handle_table();
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

/// The handle, then the VMO.
///
/// Resolving the handle first is what puts BadHandle > BadType >
/// AccessDenied ahead of every argument error (design 4.5), and it applies
/// to a zero-length read too -- a handle without Read is refused whatever
/// the length. Everything about the range, the buffer and the copy is the
/// object's (VmoDispatcher::read_user()); this layer reaches nothing below
/// kernel/object.
auto vmo_read(ours::Uandle const vmo, user_copy::UserOutPtr<u8> const buffer, u64 const offset,
              usize const length) -> Status {
    ustl::Arc<VmoDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            vmo, Rights::Read, &dispatcher);
        status != Status::Ok) {
        return status;
    }
    return dispatcher->read_user(buffer, offset, length);
}

/// A child of |vmo|'s VMO -- a slice or a snapshot -- published as vmo_create
/// does (design 4.1).
///
/// The handle must grant Duplicate and Read: a child is a second way into the
/// same bytes, so it takes what a duplicate takes plus the right to read them.
///
/// A slice's rights are the parent handle's, unchanged: never Write added, nor
/// SetProperty (design §10). A slice writes the parent's own pages, so a slice
/// of a read-only BootFS has to be read-only, which GetBackingMemory relies on
/// alongside its own handle_duplicate.
///
/// A snapshot's are the parent handle's with Write added and Execute taken
/// away. Write, because a snapshot is made to be written -- a program's data
/// segment, cloned from a read-only file -- and a write to it can only ever
/// reach its own copy of a page, never the parent's. Execute away, because an
/// object a program may write is not one it may also run without asking: code
/// that can be written then executed goes through vmo_replace_as_executable().
/// Every snapshot kind gets this; an option asking for a snapshot without Write
/// (NoWrite) is not taken here yet.
auto vmo_create_child(ours::Uandle const vmo, VmoChildOptions const options, u64 const offset,
                      u64 const size, user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
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

    if (VmoDispatcher::is_snapshot(options)) {
        rights &= ~Rights::Execute;
        rights |= Rights::Write;
    }

    auto handle = object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(child)), rights);
    if (!handle) {
        return Status::NoResource;
    }
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

/// A new handle to |vmo|'s VMO with Execute and without Write, in place of
/// |vmo| -- which this consumes whatever the answer (design D3, K19).
///
/// Step by step:
///   1. vmex is checked first and its answer kept: a System resource covering
///      the VMEX slot, or ours::Uandle::Invalid, which is AccessDenied: there
///      is no ambient Execute to allow without the resource.
///   2. |vmo| is taken out of the table. From here it is gone, whether or not
///      the call succeeds; a caller that wants to keep a copy duplicates it
///      first (handle_duplicate). BadHandle when it names nothing, and also
///      when it names something that is not a VMO -- the handle is already
///      consumed by then.
///   3. vmex's answer, now.
///   4. The new handle: the old rights less Write, plus Execute. Write is
///      dropped (D3) so that a program cannot write the code it is about to
///      run through the handle that lets it run it, though a JIT would want
///      to keep it.
/// The value goes out before the handle goes in, as for vmo_create.
auto vmo_replace_as_executable(ours::Uandle const vmo, ours::Uandle const vmex,
                               user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    auto vmex_status = Status::AccessDenied;
    if (vmex != ours::Uandle::Invalid) {
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
    auto handle = object::Handle::make(original->dispatcher(), rights);
    if (!handle) {
        return Status::NoResource;
    }
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

/// vmo_read()'s mirror: the handle with Write, then the VMO.
auto vmo_write(ours::Uandle const vmo, user_copy::UserInPtr<u8 const> const buffer,
               u64 const offset, usize const length) -> Status {
    ustl::Arc<VmoDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            vmo, Rights::Write, &dispatcher);
        status != Status::Ok) {
        return status;
    }
    return dispatcher->write_user(buffer, offset, length);
}

/// The handle, then the size, then the copy out.
///
/// No right is asked of the handle (Rights::None): anyone should be able to get
/// the size. The size says nothing about the bytes, and a holder that may not
/// read them still needs it to know what it holds -- the launcher, which takes
/// a program's VMO and must bound its parsing by it, is the first caller
/// (launch::Plan::build()). The handle is still resolved first, so BadHandle
/// and BadType win over a bad destination (design 4.5).
auto vmo_get_size(ours::Uandle const vmo, user_copy::UserOutPtr<u64> const size) -> Status {
    ustl::Arc<VmoDispatcher> dispatcher;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            vmo, Rights::None, &dispatcher);
        status != Status::Ok) {
        return status;
    }
    return size.copy_to_user(dispatcher->size());
}
} // namespace ours::syscall
