/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/cfg.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_object_physical.hpp>
#include <ours/object/vmo_dispatcher.hpp>

#include <gktl/counter.hpp>
#include <heap/new.hpp>

GKTL_COUNTER(DISPATCHER_VMO_CREATE_COUNT, "dispatcher.vmo.create");
GKTL_COUNTER(DISPATCHER_VMO_DESTROY_COUNT, "dispatcher.vmo.destroy");

namespace ours::object {
VmoDispatcher::VmoDispatcher(ustl::Arc<mm::VmObject> vmo) : Base(), m_vmo(ustl::move(vmo)) {
    DISPATCHER_VMO_CREATE_COUNT.add(1);
}

VmoDispatcher::~VmoDispatcher() {
    DISPATCHER_VMO_DESTROY_COUNT.add(1);
}

auto VmoDispatcher::create(ustl::Arc<mm::VmObject> vmo, KernelHandle<Self> &handle_out,
                           Rights &rights_out) -> Status {
    if (!vmo) {
        return Status::InvArgs;
    }

    // GAF_USER, as with every other dispatcher: the lifetime of this object is
    // driven by how many handles userspace holds.
    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_USER) Self(ustl::move(vmo)));
    if (!self) {
        return Status::OutOfMem;
    }

    rights_out = default_right();
    handle_out = KernelHandle<Self>(ustl::move(self));
    return Status::Ok;
}

auto VmoDispatcher::create_paged(usize const size, KernelHandle<Self> &handle_out,
                                 Rights &rights_out) -> Status {
    // Pinned, and not lazy, because nothing here can service a fault on it
    // yet: userboot's stack has to be resident the moment ring 3 touches it.
    // This is the flag mm::VmArea::map_anon() already passes for the same
    // reason, so this is the existing behaviour spelled out rather than a new
    // policy.
    ustl::Arc<mm::VmObjectPaged> vmo;
    auto const status = mm::VmObjectPaged::create(mm::GAF_USER, size, mm::VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return status;
    }

    return create(ustl::move(vmo), handle_out, rights_out);
}

auto VmoDispatcher::create_physical(PhysAddr const base, usize const size,
                                    KernelHandle<Self> &handle_out, Rights &rights_out) -> Status {
    ustl::Arc<mm::VmObjectPhysical> vmo;
    auto const status = mm::VmObjectPhysical::create(base, size, mm::VmoFlags::Pinned, &vmo);
    if (Status::Ok != status) {
        return status;
    }

    return create(ustl::move(vmo), handle_out, rights_out);
}

/// The checks that belong to the call, then straight to the VMO.
///
/// The bytes go from the VMO's page to the user buffer in one copy, under the
/// VMO's lock, with no kernel buffer in between; mm::VmObjectPaged::read_user()
/// copies with fault capture and drops the lock to resolve a fault on the user
/// side, so holding the lock across the copy is safe. This used to go a page at
/// a time through a kernel bounce page -- read into it under the lock, then copy
/// out with no lock held -- because the copy could not yet report a fault
/// instead of taking one. That cost a memcpy per page and a page of kmalloc per
/// call, and bought nothing capture does not.
///
/// Where the answers can differ from Zircon's:
///
///   - A buffer that cannot take the bytes is InvArgs, which is what
///     user_copy answers everywhere (design 4.6). Zircon says NotFound when
///     it is outside the user half or unmapped, and AccessDenied when it is
///     read-only.
///   - A physical VMO. Zircon's VmObject::ReadUser() answers NotSupported
///     before looking at the range or the buffer; here the range and user-half
///     checks come first and can answer OutOfRange or InvArgs instead. Latent:
///     create_physical() has no caller, so no physical VMO reaches user space
///     yet.
auto VmoDispatcher::read_user(user_copy::UserOutPtr<u8> const out, u64 const offset,
                              usize const length) -> Status {
    // Written so that nothing overflows: |length| is bounded by the size
    // first, and only then subtracted from it. mm checks the range again under
    // the VMO's lock; this one only has to be right about which answer an
    // out-of-range request gets before anything else is looked at.
    auto const size = m_vmo->size();
    if (length > size || offset > size - length) {
        return Status::OutOfRange;
    }

    if (length == 0) {
        // Asked of the VMO rather than answered here, so that one which cannot
        // be read says NotSupported for an empty read too. |out| is never
        // looked at: a paged VMO answers a zero-byte read with Ok before
        // anything else (mm::VmObjectPaged::read_write()).
        return m_vmo->read_user(out, VirtAddr(offset), 0);
    }

    // The whole buffer, before the first page is read: reading a page nobody
    // has written commits a zeroed one (there is no shared zero page yet), and
    // a call that is bound to fail should not cost the VMO memory.
    if (!user_copy::is_user_range(out.address(), length)) {
        return Status::InvArgs;
    }

    return m_vmo->read_user(out, VirtAddr(offset), length);
}
} // namespace ours::object
