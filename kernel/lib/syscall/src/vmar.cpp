/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/vmar_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

namespace ours::syscall {
namespace {
    /// Every bit VmOptions names. Anything else is not an option at all
    /// (InvArgs); a named one the dispatcher does not act on yet is
    /// NotSupported, from VmarDispatcher::map().
    constexpr auto KNOWN_VM_OPTIONS = static_cast<VmOptions>((1U << 15) - 1);

    /// The bits the kernel sets from the handles' rights, and a program may
    /// not: they are the mapping's ceiling, and a ceiling the program chose
    /// would be no ceiling. Zircon refuses them the same way.
    constexpr auto CAN_MAP = VmOptions::CanMapSpecific | VmOptions::CanMapRead |
                             VmOptions::CanMapWrite | VmOptions::CanMapExecute;
} // namespace

/// Maps [vmo_offset, vmo_offset + length) of |vmo| into |vmar| and writes the
/// address to |mapped_addr|.
///
/// Zircon's sys_vmar_map(), in its order:
///   1. both handles, with their rights (BadHandle, BadType);
///   2. Map on the VMO's handle (AccessDenied);
///   3. the options: known bits only, no CanMap*, and a protection this
///      kernel allows (InvArgs) -- Write and Execute each need Read, and
///      never both at once (W^X, design D3; Zircon allows both and allows
///      execute-only);
///   4. each Perm bit against the two handles: PermRead needs Read on both,
///      PermWrite Write, PermExecute Execute (AccessDenied);
///   5. every right both handles grant becomes a CanMap bit -- the mapping's
///      ceiling, which a later vmar_protect is held to (interface design 2.3);
///   6. the mapping (VmarDispatcher::map(): offsets, sizes, placement).
/// The address can only be known once the mapping exists, so it cannot go out
/// before the mapping is made, as a handle value does (design 4.1). Instead a
/// write that fails takes the mapping back down: a program never ends up with
/// memory mapped that it was told was not.
/// Reserves [offset, offset + size) of |parent| as a sub-region, and writes
/// its handle to |child| and its address to |child_addr|.
///
/// Zircon's sys_vmar_allocate(), in its order: the rights each CanMap bit
/// would need of the parent (Read, Write, Execute), the parent with those
/// rights (BadHandle, BadType, AccessDenied), then the region
/// (VmarDispatcher::allocate(): only Specific without CanMap today). A bit
/// VmOptions does not name is InvArgs before any of that.
///
/// Two values go out, and both before the handle goes into the table (design
/// 4.1), so a program never holds a handle it was told it did not get. If
/// either write fails, the sub-region is taken down again: closing the handle
/// alone would leave it reserved, since the parent region holds it.
auto vmar_allocate(HandleValue const parent, VmOptions const options, usize const offset,
                   usize const size, user_copy::UserOutPtr<HandleValue> const child,
                   user_copy::UserOutPtr<VirtAddr> const child_addr) -> Status {
    if ((options & ~KNOWN_VM_OPTIONS) != VmOptions::None) {
        return Status::InvArgs;
    }

    auto required = Rights::None;
    if (!!(options & VmOptions::CanMapRead)) {
        required |= Rights::Read;
    }
    if (!!(options & VmOptions::CanMapWrite)) {
        required |= Rights::Write;
    }
    if (!!(options & VmOptions::CanMapExecute)) {
        required |= Rights::Execute;
    }

    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Arc<object::VmarDispatcher> region;
    if (auto const status = table.get_dispatcher(parent, required, &region); status != Status::Ok) {
        return status;
    }

    object::KernelHandle<object::VmarDispatcher> sub;
    auto rights = Rights::None;
    VirtAddr base = 0;
    if (auto const status = region->allocate(options, offset, size, sub, rights, base);
        status != Status::Ok) {
        return status;
    }

    auto handle =
        object::Handle::make(object::KernelHandle<object::Dispatcher>(ustl::move(sub)), rights);
    if (!handle) {
        static_cast<void>(region->unmap(base, size));
        return Status::NoResource;
    }
    auto status = child.copy_to_user(table.value_of(*handle));
    if (status == Status::Ok) {
        status = child_addr.copy_to_user(base);
    }
    if (status != Status::Ok) {
        static_cast<void>(region->unmap(base, size));
        return status;
    }
    static_cast<void>(table.add(ustl::move(handle)));
    return Status::Ok;
}

auto vmar_map(HandleValue const vmar, VmOptions const options, usize const vmar_offset,
              HandleValue const vmo, u64 const vmo_offset, usize const length,
              user_copy::UserOutPtr<VirtAddr> const mapped_addr) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Arc<object::VmarDispatcher> region;
    auto vmar_rights = Rights::None;
    if (auto const status =
            table.get_dispatcher_with_rights(vmar, Rights::None, &region, &vmar_rights);
        status != Status::Ok) {
        return status;
    }
    ustl::Arc<object::VmoDispatcher> object;
    auto vmo_rights = Rights::None;
    if (auto const status =
            table.get_dispatcher_with_rights(vmo, Rights::None, &object, &vmo_rights);
        status != Status::Ok) {
        return status;
    }

    if (!(vmo_rights & Rights::Map)) {
        return Status::AccessDenied;
    }

    if ((options & ~KNOWN_VM_OPTIONS) != VmOptions::None ||
        (options & CAN_MAP) != VmOptions::None) {
        return Status::InvArgs;
    }
    auto const read = !!(options & VmOptions::PermRead);
    auto const write = !!(options & VmOptions::PermWrite);
    auto const execute = !!(options & VmOptions::PermExecute);
    if ((write || execute) && !read) {
        return Status::InvArgs;
    }
    if (write && execute) {
        return Status::InvArgs;
    }

    auto const both = vmar_rights & vmo_rights;
    auto const can_read = !!(both & Rights::Read);
    auto const can_write = !!(both & Rights::Write);
    auto const can_execute = !!(both & Rights::Execute);
    if ((read && !can_read) || (write && !can_write) || (execute && !can_execute)) {
        return Status::AccessDenied;
    }

    auto with_ceiling = options;
    if (can_read) {
        with_ceiling |= VmOptions::CanMapRead;
    }
    if (can_write) {
        with_ceiling |= VmOptions::CanMapWrite;
    }
    if (can_execute) {
        with_ceiling |= VmOptions::CanMapExecute;
    }

    auto const mapped =
        region->map(with_ceiling, vmar_offset, *object, vmo_offset, length, "vmar_map");
    if (mapped.is_err()) {
        return mapped.unwrap_err();
    }
    if (auto const status = mapped_addr.copy_to_user(mapped.unwrap()); status != Status::Ok) {
        static_cast<void>(region->unmap(mapped.unwrap(), length));
        return status;
    }
    return Status::Ok;
}
} // namespace ours::syscall
