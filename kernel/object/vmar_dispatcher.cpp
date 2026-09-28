/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <gktl/counter.hpp>
#include <heap/new.hpp>
// The mapping entry points hand back an Arc<VmMapping>, and destroying an
// Arc needs the pointee's definition, not just its declaration -- vm_area.hpp
// only forward-declares VmMapping.
#include <ours/mm/vm_mapping.hpp>
#include <ours/object/vmar_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>

#include <ustl/mem/align.hpp>

GKTL_COUNTER(DISPATCHER_VMAR_CREATE_COUNT, "dispatcher.vmar.create");
GKTL_COUNTER(DISPATCHER_VMAR_DESTROY_COUNT, "dispatcher.vmar.destroy");

namespace ours::object {
VmarDispatcher::VmarDispatcher(ustl::Arc<mm::VmArea> vma) : Base(), m_vma(ustl::move(vma)) {
    DISPATCHER_VMAR_CREATE_COUNT.add(1);
}

VmarDispatcher::~VmarDispatcher() {
    DISPATCHER_VMAR_DESTROY_COUNT.add(1);
}

auto VmarDispatcher::create(ustl::Arc<mm::VmArea> vma, KernelHandle<Self> &handle_out,
                            Rights &rights_out) -> Status {
    if (!vma) {
        return Status::InvArgs;
    }

    // GAF_USER, not GAF_KERNEL: a dispatcher's lifetime is driven by how many
    // handles userspace holds, so its memory is charged to the same pool as
    // everything else userspace can cause the kernel to allocate.
    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_USER) Self(ustl::move(vma)));
    if (!self) {
        return Status::OutOfMem;
    }

    // Read, Write and Execute from what the region itself allows, as Zircon's
    // VmAddressRegionDispatcher::Create() adds them from the VMAR's own
    // CAN_MAP_READ / WRITE / EXECUTE: a region that cannot map executable
    // pages never hands out a handle claiming it can, and one that can -- a
    // process's root region -- hands out a handle that lets vmar_map ask.
    auto rights = default_right();
    auto const allowed = self->m_vma->permissions();
    if (!!(allowed & mm::VmaFlags::Read)) {
        rights |= Rights::Read;
    }
    if (!!(allowed & mm::VmaFlags::Write)) {
        rights |= Rights::Write;
    }
    if (!!(allowed & mm::VmaFlags::Exec)) {
        rights |= Rights::Execute;
    }
    rights_out = rights;
    handle_out = KernelHandle<Self>(ustl::move(self));
    return Status::Ok;
}

auto VmarDispatcher::map(VmOptions const options, usize const vmar_offset, VmoDispatcher &vmo,
                         u64 const vmo_offset, usize const length, char const *const name)
    -> ktl::Result<VirtAddr> {
    verify_canary();
    vmo.verify_canary();

    constexpr auto HANDLED = VmOptions::PermRead | VmOptions::PermWrite | VmOptions::PermExecute |
                             VmOptions::Specific | VmOptions::CanMapRead | VmOptions::CanMapWrite |
                             VmOptions::CanMapExecute | VmOptions::MapRange;
    if ((options & ~HANDLED) != VmOptions::None) {
        return ktl::err(Status::NotSupported);
    }

    if (length == 0 || !ustl::mm::is_aligned(vmo_offset, PAGE_SIZE)) {
        return ktl::err(Status::InvArgs);
    }
    auto const rounded = ustl::mm::align_up(length, PAGE_SIZE);
    if (rounded == 0) {
        // Rounding wrapped: no VMO is that big, and no region either.
        return ktl::err(Status::OutOfRange);
    }

    // Each Perm bit needs its CanMap bit: a mapping may not start above its
    // own ceiling. Zircon answers ZX_ERR_ACCESS_DENIED the same way, from
    // VmAddressRegion::CreateVmMapping().
    auto const asks = [options](VmOptions perm, VmOptions can) {
        return !!(options & perm) && !(options & can);
    };
    if (asks(VmOptions::PermRead, VmOptions::CanMapRead) ||
        asks(VmOptions::PermWrite, VmOptions::CanMapWrite) ||
        asks(VmOptions::PermExecute, VmOptions::CanMapExecute)) {
        return ktl::err(Status::AccessDenied);
    }

    // Inside the VMO, written so that it cannot wrap.
    auto const vmo_size = u64(vmo.vmo()->size());
    if (rounded > vmo_size || vmo_offset > vmo_size - rounded) {
        return ktl::err(Status::OutOfRange);
    }

    // Where: vmar_offset counts from this region's start, and only means
    // something with Specific -- without it the kernel picks, and an offset
    // would be a request nobody reads. Zircon refuses that as well.
    auto where = mm::VmPlacement::anywhere();
    if (!!(options & VmOptions::Specific)) {
        if (!ustl::mm::is_aligned(vmar_offset, PAGE_SIZE) || vmar_offset >= m_vma->size()) {
            return ktl::err(Status::InvArgs);
        }
        where = mm::VmPlacement::at(m_vma->base() + vmar_offset);
    } else if (vmar_offset != 0) {
        return ktl::err(Status::InvArgs);
    }

    // The options in mm's words. User always: everything mapped through a
    // VMAR handle is for ring 3.
    auto mmuf = mm::MmuFlags::User;
    auto ceiling = mm::VmaFlags::None;
    if (!!(options & VmOptions::PermRead)) {
        mmuf |= mm::MmuFlags::Readable;
    }
    if (!!(options & VmOptions::PermWrite)) {
        mmuf |= mm::MmuFlags::Writable;
    }
    if (!!(options & VmOptions::PermExecute)) {
        mmuf |= mm::MmuFlags::Executable;
    }
    if (!!(options & VmOptions::CanMapRead)) {
        ceiling |= mm::VmaFlags::Read;
    }
    if (!!(options & VmOptions::CanMapWrite)) {
        ceiling |= mm::VmaFlags::Write;
    }
    if (!!(options & VmOptions::CanMapExecute)) {
        ceiling |= mm::VmaFlags::Exec;
    }
    auto const populate =
        !!(options & VmOptions::MapRange) ? mm::VmMapOption::Commit : mm::VmMapOption::None;

    // map_with_vmo(), not create_mapping(). create_mapping() builds the
    // mapping object and stops there: it accepts a VmMapOption and never acts
    // on Commit, so the mapping comes back live with no page tables installed.
    // Written against it, userboot's image mapped "successfully" and then took
    // a not-present instruction fetch on its very first instruction
    // (error_code=0x14, rip=0x201270, cs=0x33). Committing the range,
    // installing the PTEs, and unwinding correctly when that fails is
    // map_with_vmo's job, and none of it is worth reproducing here.
    //
    // The VMO Arc is copied rather than moved: mapping does not consume a VMO,
    // which is the point of it being a separate object -- the handle the
    // caller holds goes on naming the same memory afterwards.
    auto mapping =
        m_vma->map_with_vmo(where, rounded, vmo_offset, mmuf, ceiling, vmo.vmo(), populate, name);
    if (mapping.is_err()) {
        return ktl::err(mapping.unwrap_err());
    }
    return ktl::ok(mapping.unwrap()->base());
}

} // namespace ours::object
