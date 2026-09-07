/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <gktl/counter.hpp>
#include <ktl/new.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_object_physical.hpp>
#include <ours/object/vmo_dispatcher.hpp>

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

} // namespace ours::object
