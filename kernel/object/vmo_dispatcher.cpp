/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/cfg.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_object_physical.hpp>
#include <ours/object/vmo_dispatcher.hpp>

#include <algorithm>

#include <gktl/counter.hpp>
#include <heap/new.hpp>
#include <ustl/box.hpp>

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

namespace {
    /// The kernel page read_user() passes the bytes through.
    ///
    /// A type of its own so that ustl::Box can own it: the storage comes from
    /// `new (GAF_KERNEL)`, which serves exactly one page from kmalloc's top
    /// size class, and the Box gives it back on every way out. Not on the
    /// stack: a thread's kernel stack is four pages, and a fault taken while
    /// copying out nests on that same stack (#PF has no IST).
    struct Bounce {
        u8 bytes[PAGE_SIZE];
    };
    static_assert(sizeof(Bounce) == PAGE_SIZE);
} // namespace

/// A page at a time, through one kernel page: under the VMO's lock into the
/// bounce page (mm::VmObject::read() takes and drops it), then, with no lock
/// held, out to the user.
///
/// The two halves cannot be one copy. A copy to user memory can fault, and
/// the fault path must never find this thread holding the VMO's lock: once
/// the address space's fault path takes the VMO's lock as Zircon's does
/// (docs/repairs/260925/mm.md), a user buffer mapped from the same VMO would
/// deadlock against itself. Stepping by page also bounds what one hold of the
/// lock covers, whatever length a program asks for; the steps are aligned to
/// the VMO's pages, so each touches exactly one of them.
///
/// Zircon does not bounce. VmObjectPaged::ReadUser() copies straight from the
/// page to the user under the VMO's lock, with a copy that reports a fault
/// instead of taking it (copy_array_to_user_capture_faults), and on a fault
/// drops the lock, faults the page in and resumes. This kernel has no
/// capture-faults copy -- its copy takes the fault, and resolving it goes
/// through the page-fault path -- so the lock has to be gone before the copy
/// starts, and a kernel page is what carries the bytes across the gap. The
/// cost is one extra memcpy per page.
///
/// Where the answers can differ from Zircon's:
///
///   - A buffer that cannot take the bytes is InvArgs, which is what
///     user_copy answers everywhere (design 4.6). Zircon says NotFound when
///     it is outside the user half or unmapped, and AccessDenied when it is
///     read-only.
///   - A physical VMO. Zircon's VmObject::ReadUser() answers NotSupported
///     before looking at the range or the buffer; here it comes from
///     mm::VmObject::read(), so the range check, the user-half check and the
///     bounce allocation come first and can answer OutOfRange, InvArgs or
///     NoMemory instead. Latent: create_physical() has no caller, so no
///     physical VMO reaches user space yet.
auto VmoDispatcher::read_user(user_copy::UserOutPtr<u8> const out, u64 const offset,
                              usize const length) -> Status {
    // Written so that nothing overflows: |length| is bounded by the size
    // first, and only then subtracted from it. The size is read once and
    // trusted for the rest of the call, which holds while nothing can resize
    // a VMO. When something can, a shrink between two pages makes the later
    // page's own check in mm::VmObject::read() answer OutOfRange after the
    // earlier pages were delivered -- what Zircon does in the same race.
    auto const size = m_vmo->size();
    if (length > size || offset > size - length) {
        return Status::OutOfRange;
    }

    if (length == 0) {
        // Asked of the VMO rather than answered here, so that one which cannot
        // be read says NotSupported for an empty read too. |out| is never
        // looked at: a paged VMO answers a zero-byte read with Ok before
        // anything else (mm::VmObjectPaged::read()).
        return m_vmo->read(nullptr, VirtAddr(offset), 0);
    }

    // The whole buffer, before the first page is read: reading a page nobody
    // has written commits a zeroed one here (there is no shared zero page
    // yet), and a call that is bound to fail should not cost the VMO memory.
    if (!user_copy::is_user_range(out.address(), length)) {
        return Status::InvArgs;
    }

    auto bounce = ustl::Box<Bounce>::from_raw(new (mm::GAF_KERNEL) Bounce);
    if (!bounce) {
        return Status::NoMemory;
    }

    usize done = 0;
    while (done < length) {
        // Up to the end of the VMO page this step starts in: the first step
        // may begin mid-page, every later one begins on a page boundary.
        auto const at = offset + done;
        auto const step = std::min<usize>(length - done, PAGE_SIZE - at % PAGE_SIZE);

        if (auto const status = m_vmo->read(bounce->bytes, VirtAddr(at), step);
            status != Status::Ok) {
            return status;
        }
        if (auto const status = out.element_offset(done).copy_array_to_user(bounce->bytes, step);
            status != Status::Ok) {
            return status;
        }
        done += step;
    }
    return Status::Ok;
}
} // namespace ours::object
