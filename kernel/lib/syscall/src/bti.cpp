/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <algorithm>
#include <span>

#include <ours/object/bti_dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/iommu_dispatcher.hpp>
#include <ours/object/pmt_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/resource_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::syscall {
using object::BtiDispatcher;
using object::IommuDispatcher;
using object::KernelHandle;
using object::PmtDispatcher;
using object::ProcessDispatcher;
using object::VmoDispatcher;

namespace {
    /// How many addresses are copied out at a time, from the stack.
    constexpr usize ADDR_CHUNK = 32;

    /// |object|'s handle, its value out to the caller, and only then the handle
    /// into the table -- vmo_create's order (design 4.1), so that a failure at
    /// any step leaves nothing the program could have seen.
    template <typename T>
    auto publish(KernelHandle<T> object, Rights const rights,
                 user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
        auto handle =
            object::Handle::make(KernelHandle<object::Dispatcher>(ustl::move(object)), rights);
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
} // namespace

/// The resource first: BadHandle, BadType and OutOfRange come before anything
/// about |type|. The System slot is the whole permission, as
/// VMEX's is for vmo_replace_as_executable. Everything about |type| is the
/// object's (IommuDispatcher::create()).
auto iommu_create(ours::Uandle const resource, IommuType const type,
                  user_copy::UserInPtr<u8 const> const desc, usize const desc_size,
                  user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    if (auto const status =
            object::validate_ranged_resource(resource, RsrcKind::System, RSRC_SYSTEM_IOMMU_BASE, 1);
        status != Status::Ok) {
        return status;
    }
    // A descriptor is the hardware's, and bounds a size the program chooses.
    if (desc_size > IOMMU_DESC_MAX_SIZE) {
        return Status::InvArgs;
    }

    KernelHandle<IommuDispatcher> iommu;
    Rights rights = Rights::None;
    if (auto const status = IommuDispatcher::create(type, desc_size, iommu, rights);
        status != Status::Ok) {
        return status;
    }
    return publish(ustl::move(iommu), rights, out);
}

/// The IOMMU needs no right: it is the capability to make BTIs, and holding
/// a handle to one is that.
auto bti_create(ours::Uandle const iommu, u32 const options, u64 const bti_id,
                user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    ustl::Arc<IommuDispatcher> iommu_object;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            iommu, Rights::None, &iommu_object);
        status != Status::Ok) {
        return status;
    }
    // None is defined yet: room is left for them.
    if (options != 0) {
        return Status::InvArgs;
    }

    KernelHandle<BtiDispatcher> bti;
    Rights rights = Rights::None;
    if (auto const status = BtiDispatcher::create(ustl::move(iommu_object), bti_id, bti, rights);
        status != Status::Ok) {
        return status;
    }
    return publish(ustl::move(bti), rights, out);
}

/// The two handles, then the rights they carry for what the device may do, then
/// the arguments, then the pin (design 4.5: BadHandle > BadType > AccessDenied
/// before any argument error).
///
/// The number of addresses is worked out and checked before anything is pinned,
/// so that the commonest mistake, a buffer of the wrong size, costs nothing.
///
/// From the pin on, a failure unpins before it returns, and what was already
/// written to |addrs| is left there: the caller is told nothing is pinned. The pin's handle is not
/// in a table, and a handle that is dropped is a last handle closed, which the
/// BTI reads as a program that went away with memory in reach: it would
/// quarantine the pin, and the BTI would refuse every pin after it. The token
/// is made into a handle from a plain reference (Handle::make() of an Arc), not
/// from a KernelHandle, for the same reason: a make() that refuses must not
/// close the last handle of an object it never gave anyone.
auto bti_pin(ours::Uandle const bti, BtiPinOptions const options, ours::Uandle const vmo,
             u64 const offset, u64 const size, user_copy::UserOutPtr<PhysAddr> const addrs,
             usize const addrs_count, user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    auto &table = ProcessDispatcher::current()->handle_table();

    ustl::Arc<BtiDispatcher> bti_object;
    if (auto const status = table.get_dispatcher(bti, Rights::Map, &bti_object);
        status != Status::Ok) {
        return status;
    }
    ustl::Arc<VmoDispatcher> vmo_object;
    Rights vmo_rights = Rights::None;
    if (auto const status =
            table.get_dispatcher_with_rights(vmo, Rights::Map, &vmo_object, &vmo_rights);
        status != Status::Ok) {
        return status;
    }

    // What the device may do with the memory is limited by what this program
    // may do with it. Execute is not asked for PermExecute: the right is the
    // CPU's, the permission is the device's.
    if (!!(options & (BtiPinOptions::PermRead | BtiPinOptions::PermExecute)) &&
        !(vmo_rights & Rights::Read)) {
        return Status::AccessDenied;
    }
    if (!!(options & BtiPinOptions::PermWrite) && !(vmo_rights & Rights::Write)) {
        return Status::AccessDenied;
    }

    BtiDispatcher::Pieces pieces{};
    if (auto const status = bti_object->pieces(options, size, pieces); status != Status::Ok) {
        return status;
    }
    if (addrs_count != pieces.count) {
        return Status::InvArgs;
    }
    // The size of the pin, not only the number of addresses: with Contiguous
    // the whole pin is one address, so a count of 1 would bound nothing. Every
    // page of the range is committed and pinned before anything can fail.
    if (size / PAGE_SIZE > BTI_PIN_MAX_PAGES) {
        return Status::InvArgs;
    }

    KernelHandle<PmtDispatcher> pinned;
    Rights rights = Rights::None;
    if (auto const status =
            bti_object->pin(vmo_object->vmo(), offset, size, options, pinned, rights);
        status != Status::Ok) {
        return status;
    }
    // Out of the KernelHandle, whose destructor would close the last handle.
    auto token = pinned.release();

    // The handle before the addresses: a program at its quota pays for the pin
    // and is not left holding addresses of pages that are no longer pinned.
    auto handle = object::Handle::make(ustl::Arc<object::Dispatcher>(token), rights);
    if (!handle) {
        token->unpin();
        return Status::NoResource;
    }

    PhysAddr chunk[ADDR_CHUNK];
    for (usize first = 0; first < addrs_count; first += ADDR_CHUNK) {
        auto const count = std::min(ADDR_CHUNK, addrs_count - first);
        if (auto const status = token->query_pieces(pieces.size, first, std::span(chunk, count));
            status != Status::Ok) {
            token->unpin();
            return status;
        }
        if (auto const status = addrs.element_offset(first).copy_array_to_user(chunk, count);
            status != Status::Ok) {
            token->unpin();
            return status;
        }
    }
    if (auto const status = out.copy_to_user(table.value_of(*handle)); status != Status::Ok) {
        token->unpin();
        return status;
    }
    auto _ = table.add(ustl::move(handle));
    return Status::Ok;
}

/// The BTI first, for Map: it is the capability for memory a device is given the
/// address of, so a program with a VMO and no BTI cannot make this one.
/// Everything about the size and the alignment is the object's
/// (VmoDispatcher::create_contiguous()).
auto vmo_create_contiguous(ours::Uandle const bti, u64 const size, u32 const alignment_log2,
                           user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    ustl::Arc<BtiDispatcher> bti_object;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            bti, Rights::Map, &bti_object);
        status != Status::Ok) {
        return status;
    }

    KernelHandle<VmoDispatcher> vmo;
    Rights rights = Rights::None;
    if (auto const status = VmoDispatcher::create_contiguous(size, alignment_log2, vmo, rights);
        status != Status::Ok) {
        return status;
    }
    return publish(ustl::move(vmo), rights, out);
}

/// The handle is taken out of the table only if it names a pin (BadHandle and
/// BadType leave the table as it was), in one hold of the table's lock: a
/// handle that is looked up and then removed could be closed, and the pin
/// quarantined, by another thread in between. The unpin comes before the handle
/// is closed, so that closing finds nothing to quarantine.
auto pmt_unpin(ours::Uandle const pmt) -> Status {
    ustl::Box<object::Handle> closed;
    if (auto const status =
            ProcessDispatcher::current()->handle_table().remove_as<PmtDispatcher>(pmt, &closed);
        status != Status::Ok) {
        return status;
    }
    closed->dispatcher().downcast<PmtDispatcher>()->unpin();
    return Status::Ok;
}

auto bti_release_quarantine(ours::Uandle const bti) -> Status {
    ustl::Arc<BtiDispatcher> bti_object;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            bti, Rights::Write, &bti_object);
        status != Status::Ok) {
        return status;
    }
    bti_object->release_quarantine();
    return Status::Ok;
}
} // namespace ours::syscall
