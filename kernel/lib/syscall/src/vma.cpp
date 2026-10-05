/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

namespace ours::syscall {
namespace {
    /// Every bit VmOptions names. Anything else is not an option at all
    /// (InvArgs); a named one the dispatcher does not act on yet is
    /// NotSupported, from VmaDispatcher::map() or allocate().
    constexpr auto KNOWN_VM_OPTIONS = static_cast<VmOptions>((1U << 15) - 1) | VmOptions::Sealed;

    /// CanMapRead/Write/Execute, which the kernel sets from the handles'
    /// rights as the mapping's ceiling -- a ceiling the program chose would be
    /// no ceiling -- and CanMapSpecific, which a mapping never carries.
    /// vma_map refuses all four from a program (InvArgs); vma_allocate takes
    /// them as the sub-region's allowance.
    constexpr auto CAN_MAP = VmOptions::CanMapSpecific | VmOptions::CanMapRead |
                             VmOptions::CanMapWrite | VmOptions::CanMapExecute;
} // namespace

/// Reserves |size| bytes of |parent| as a sub-region -- at |offset| with
/// Specific, wherever the kernel finds room without -- and writes its handle
/// to |child| and its address to |child_addr|.
///
/// In order: the rights each CanMap bit would need of the parent (Read, Write,
/// Execute), the parent with those rights (BadHandle, BadType, AccessDenied),
/// then the region (VmaDispatcher::allocate(): Specific or not, CanMapRead,
/// CanMapWrite, CanMapExecute, and CanMapSpecific, which needs no right). A bit
/// VmOptions does not name is InvArgs before any of that.
///
/// Two values go out, and both before the handle goes into the table (design
/// 4.1), so a program never holds a handle it was told it did not get. If
/// either write fails, or the handle cannot be made, the sub-region is taken
/// down again: closing the handle alone would leave it reserved, since the
/// parent region holds it. Taken down by identity (undo_allocate()), the
/// sub-region this call made and not its range: from the moment it is linked
/// its range is public, and another thread holding |parent| can unmap it and
/// reserve or map something of its own at the same place before the undo
/// runs. Kernel placement makes the place easy to hit, since it takes the
/// lowest room that fits.
auto vma_allocate(ours::Uandle const parent, VmOptions const options, usize const offset,
                  usize const size, user_copy::UserOutPtr<ours::Uandle> const child,
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
    ustl::Arc<object::VmaDispatcher> region;
    if (auto const status = table.get_dispatcher(parent, required, &region); status != Status::Ok) {
        return status;
    }

    object::KernelHandle<object::VmaDispatcher> sub;
    auto rights = Rights::None;
    VirtAddr base = 0;
    if (auto const status = region->allocate(options, offset, size, sub, rights, base);
        status != Status::Ok) {
        return status;
    }

    // The sub-region itself, kept past the move into the handle below, to
    // undo by. Not const: ustl::Arc passes its own constness through to the
    // dispatcher. Each undo's answer is ignored: the call has failed either
    // way, and BadState only says another thread took the sub-region down
    // first.
    auto made = sub.dispatcher();
    auto handle =
        object::Handle::make(object::KernelHandle<object::Dispatcher>(ustl::move(sub)), rights);
    if (!handle) {
        auto _ = region->undo_allocate(*made);
        return Status::NoResource;
    }
    auto status = child.copy_to_user(table.value_of(*handle));
    if (status == Status::Ok) {
        status = child_addr.copy_to_user(base);
    }
    if (status != Status::Ok) {
        auto _ = region->undo_allocate(*made);
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

/// Maps [vmo_offset, vmo_offset + length) of |vmo| into |vma| and writes the
/// address to |mapped_addr|.
///
/// In order:
///   1. both handles, with their rights (BadHandle, BadType);
///   2. Map on the VMO's handle (AccessDenied);
///   3. the options: known bits only, no CanMap*, and a protection this
///      kernel allows (InvArgs) -- Write and Execute each need Read, and
///      never both at once (W^X, design D3);
///   4. each Perm bit against the two handles: PermRead needs Read on both,
///      PermWrite Write, PermExecute Execute (AccessDenied);
///   5. every right both handles grant becomes a CanMap bit -- the mapping's
///      ceiling, which a later vma_protect is held to (interface design 2.3);
///   6. the mapping (VmaDispatcher::map(): offsets, sizes, placement).
/// The address can only be known once the mapping exists, so it cannot go out
/// before the mapping is made, as a handle value does (design 4.1). Instead a
/// write that fails takes the mapping back down: a program never ends up with
/// memory mapped that it was told was not. By identity (undo_map()), the
/// mapping this call made and not its range, which another thread holding
/// |vma| can unmap and map over before the undo runs. A Sealed mapping goes
/// too, since nobody was told of it, and the vDSO's code takes its
/// registration with it, so that a retry with an address the kernel can write
/// is the vDSO's first mapping again.
auto vma_map(ours::Uandle const vma, VmOptions const options, usize const vma_offset,
             ours::Uandle const vmo, u64 const vmo_offset, usize const length,
             user_copy::UserOutPtr<VirtAddr> const mapped_addr) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Arc<object::VmaDispatcher> region;
    auto vma_rights = Rights::None;
    if (auto const status =
            table.get_dispatcher_with_rights(vma, Rights::None, &region, &vma_rights);
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

    auto const both = vma_rights & vmo_rights;
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

    auto mapped = region->map(with_ceiling, vma_offset, *object, vmo_offset, length, "vma_map");
    if (mapped.is_err()) {
        return mapped.unwrap_err();
    }
    if (auto const status = mapped_addr.copy_to_user(mapped->base()); status != Status::Ok) {
        // The answer is ignored: the call has failed either way, and BadState
        // only says another thread took the mapping down first.
        auto _ = region->undo_map(ustl::move(mapped).unwrap());
        return status;
    }
    return Status::Ok;
}

/// Unmaps whatever lies in [addr, addr + length) of |vma|: mappings lose the
/// pages inside the range, and sub-regions wholly inside it are destroyed.
///
/// In order: the handle, with no right asked (BadHandle, BadType); then the
/// region (VmaDispatcher::unmap(): the address and the length, the region's
/// state, the range, sealed mappings, sub-regions). The handle's OpChildren
/// right decides whether a sub-region wholly inside the range may go, and is
/// read under the same lookup that found the region.
///
/// Nothing goes out to the program, so there is no write-back to undo: a
/// refusal tears nothing down, and Ok has torn down all of it.
auto vma_unmap(ours::Uandle const vma, VirtAddr const addr, usize const length) -> Status {
    auto &table = object::ProcessDispatcher::current()->handle_table();
    ustl::Arc<object::VmaDispatcher> region;
    auto rights = Rights::None;
    if (auto const status = table.get_dispatcher_with_rights(vma, Rights::None, &region, &rights);
        status != Status::Ok) {
        return status;
    }
    return region->unmap(addr, length, object::VmaDispatcher::op_children_from_rights(rights));
}
} // namespace ours::syscall
