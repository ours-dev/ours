/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/cfg.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vm_object_physical.hpp>
#include <ours/object/vmo_dispatcher.hpp>

#include <utility>

#include <gktl/counter.hpp>
#include <heap/new.hpp>
#include <ustl/mem/align.hpp>

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

auto VmoDispatcher::create_from_wired_pages(PhysAddr const base, usize const size,
                                            KernelHandle<Self> &handle_out, Rights &rights_out)
    -> Status {
    ustl::Arc<mm::VmObjectPaged> vmo;
    // The user class, as every VMO user space can reach: the create_committed()
    // this replaced used it too. A snapshot of the boot image allocates its
    // copies with it, and the kernel class would spin in the allocator's slow
    // path when memory runs out instead of answering OutOfMem.
    auto const status = mm::VmObjectPaged::create_from_wired_pages(mm::GAF_USER, base, size, &vmo);
    if (Status::Ok != status) {
        return status;
    }

    return create(ustl::move(vmo), handle_out, rights_out);
}

auto VmoDispatcher::create_user(u64 const size, VmoOptions const options,
                                KernelHandle<Self> &handle_out, Rights &rights_out) -> Status {
    // Options first: an unknown bit is the caller's mistake whatever the
    // size, and a known one this kernel defers is a different answer from an
    // unknown one.
    constexpr auto KNOWN = std::to_underlying(VmoOptions::Resizable);
    auto const bits = std::to_underlying(options);
    if ((bits & ~KNOWN) != 0) {
        return Status::InvArgs;
    }
    if (bits != 0) {
        return Status::NotSupported;
    }

    // Checked before rounding: MAX_SIZE leaves a page of headroom below the
    // top of the range, so anything that passes rounds up without wrapping.
    if (size > mm::VmObjectPaged::MAX_SIZE) {
        return Status::OutOfRange;
    }
    auto const rounded = ustl::mm::align_up(size, PAGE_SIZE);

    // Lazy: committed a page at a time as it is touched -- by a fault on a
    // mapping of it, or by vmo_write/vmo_read. A page nobody has written
    // reads as zero.
    ustl::Arc<mm::VmObjectPaged> vmo;
    auto const status = mm::VmObjectPaged::create(mm::GAF_USER, rounded, mm::VmoFlags::Lazy, &vmo);
    if (Status::Ok != status) {
        return status;
    }

    return create(ustl::move(vmo), handle_out, rights_out);
}

/// A slice or a copy-on-write snapshot, and only of a paged VMO: the options
/// first, then the size, then the window.
///
///   - An unknown bit is InvArgs. A known kind other than Slice or a
///     snapshot (is_snapshot()), or one of those with any other bit beside
///     it, is NotSupported (deferred, not wrong).
///   - The three snapshots are the same clone, mm::VmObjectPaged::
///     create_clone(): neither side ever sees the other's writes after it.
///       - SnapshotFull asks exactly that.
///       - SnapshotOnWrite asks less: a page the child has not written may
///         show the parent's later writes, but need not.
///       - SnapshotModified freezes every page that is not a pager's, and
///         nothing here has a pager, so it asks exactly what SnapshotFull
///         does.
///   - A physical VMO is NotSupported: a slice of one could be made, but
///     nothing here needs it yet.
///   - The size rounds up to a page, as vmo_create's does; one too large to
///     round is OutOfRange.
///   - A slice: the offset must be page-aligned and the rounded range must lie
///     inside the parent, or InvArgs -- mm::VmObjectPaged::create_child_slice().
///   - A snapshot: the offset must be page-aligned and no further than the
///     parent's end, or InvArgs, and the size may run past the end, which reads
///     as zero; a parent with pinned pages is BadState --
///     mm::VmObjectPaged::create_clone().
auto VmoDispatcher::create_child(VmoChildOptions const options, u64 const offset, u64 const size,
                                 KernelHandle<Self> &handle_out) -> Status {
    constexpr auto KNOWN = std::to_underlying(VmoChildOptions::SnapshotFull) |
                           std::to_underlying(VmoChildOptions::Resizable) |
                           std::to_underlying(VmoChildOptions::Slice) |
                           std::to_underlying(VmoChildOptions::SnapshotOnWrite) |
                           std::to_underlying(VmoChildOptions::NoWrite) |
                           std::to_underlying(VmoChildOptions::Reference) |
                           std::to_underlying(VmoChildOptions::SnapshotModified);
    auto const bits = std::to_underlying(options);
    if ((bits & ~KNOWN) != 0) {
        return Status::InvArgs;
    }
    auto const snapshot = is_snapshot(options);
    if (options != VmoChildOptions::Slice && !snapshot) {
        return Status::NotSupported;
    }

    auto *const paged = mm::as<mm::VmObjectPaged>(m_vmo.as_ptr_mut());
    if (paged == nullptr) {
        return Status::NotSupported;
    }

    // Checked before rounding, as in create_user(): MAX_SIZE leaves a page of
    // headroom, so anything that passes rounds up without wrapping.
    if (size > mm::VmObjectPaged::MAX_SIZE) {
        return Status::OutOfRange;
    }
    auto const rounded = ustl::mm::align_up(size, PAGE_SIZE);

    ustl::Arc<mm::VmObjectPaged> child;
    if (snapshot) {
        if (auto const status = paged->create_clone(offset, rounded, &child);
            Status::Ok != status) {
            return status;
        }
    } else if (auto const status = paged->create_child_slice(offset, rounded, &child);
               Status::Ok != status) {
        return status;
    }

    Rights ignored = Rights::None;
    return create(ustl::move(child), handle_out, ignored);
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
/// Two answers that are easy to get wrong:
///
///   - A buffer that cannot take the bytes is InvArgs, which is what
///     user_copy answers everywhere (design 4.6), whether it is outside the
///     user half, unmapped or read-only.
///   - A physical VMO is NotSupported, but only once the range and user-half
///     checks have passed: those come first and can answer OutOfRange or
///     InvArgs instead. Latent: create_physical() has no caller, so no
///     physical VMO reaches user space yet.
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
        // anything else (mm::VmObjectPaged::copy_page_by_page()).
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

/// read_user()'s mirror, check for check and in the same order, so that a
/// write gets the same answer as a read of the same range would: OutOfRange
/// before anything else, then an empty write, then the buffer.
auto VmoDispatcher::write_user(user_copy::UserInPtr<u8 const> const in, u64 const offset,
                               usize const length) -> Status {
    auto const size = m_vmo->size();
    if (length > size || offset > size - length) {
        return Status::OutOfRange;
    }

    if (length == 0) {
        // Asked of the VMO, as read_user() does, so that one which cannot be
        // written says NotSupported for an empty write too.
        return m_vmo->write_user(in, VirtAddr(offset), 0);
    }

    // The whole buffer first: writing commits the pages it lands on, and a
    // call that is bound to fail should not cost the VMO memory.
    if (!user_copy::is_user_range(in.address(), length)) {
        return Status::InvArgs;
    }

    return m_vmo->write_user(in, VirtAddr(offset), length);
}
} // namespace ours::object
