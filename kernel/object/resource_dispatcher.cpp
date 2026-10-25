/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/gaf.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/resource_dispatcher.hpp>
#include <ours/platform/ioport.hpp>

#include <heap/new.hpp>

namespace ours::object {
namespace {
    /// How many numbers the space of a ranged kind holds: interrupt lines are
    /// numbered by a u32 (VIrqNum), I/O ports by a u16. Zero for a kind with
    /// no ranges here: System is numbered by slot, and MmIo is not supported
    /// yet.
    auto space_of(RsrcKind const kind) -> u64 {
        switch (kind) {
            case RsrcKind::Irq:
                return u64{1} << 32;
            case RsrcKind::IoPort:
                return u64{1} << 16;
            default:
                return 0;
        }
    }

    /// Whether [base, base + size) shares a port with one the kernel drives.
    /// The caller has checked that the request lies inside the port space, so
    /// base + size cannot overflow.
    auto touches_kernel_ioports(u64 const base, u64 const size) -> bool {
        auto const end = base + size;
        for (auto const &range: platform::kernel_ioports()) {
            auto const range_end = u64{range.base} + range.count;
            if (base < range_end && u64{range.base} < end) {
                return true;
            }
        }
        return false;
    }
} // namespace

ResourceDispatcher::ResourceDispatcher(Kind kind, u64 base, u64 size, Flag flags, Name const &name)
    // Initializer order follows the member declaration order in the header.
    : Base()
    , m_name(name)
    , m_kind(kind)
    , m_flags(flags)
    , m_base(base)
    , m_size(size)
    , m_rsrclist(nullptr) {}

/// This used to check its arguments in a way no call could pass -- size 0 was
/// refused first, and then a Root resource was refused unless its size was 0
/// -- and returned Ok without ever constructing a dispatcher, leaving |out|
/// empty for a caller told it had succeeded. It now builds one, for System
/// slots and for the Irq and IoPort ranges.
auto ResourceDispatcher::create(KernelHandle<Self> *out, Rights &rights, Kind kind, u64 base,
                                u64 size, Flag flags, Name const &name, Storage *storage)
    -> Status {
    static_cast<void>(storage);
    if (!out) {
        return Status::InvArgs;
    }
    switch (kind) {
        case Kind::System:
            // One slot at a time, and only a slot this kernel knows: a size
            // of exactly 1 and a base inside the slot table.
            if (size != 1 || base >= RSRC_SYSTEM_COUNT) {
                return Status::InvArgs;
            }
            break;
        case Kind::Irq:
        case Kind::IoPort: {
            // The root, or at least one number inside the kind's space,
            // compared so that nothing overflows.
            auto const space = space_of(kind);
            auto const root = base == 0 && size == 0;
            if (!root && (size == 0 || size > space || base > space - size)) {
                return Status::InvArgs;
            }
            break;
        }
        case Kind::MmIo:
            return Status::NotSupported;
        default:
            // A number the ABI names no kind for, such as the 3 and 4 it
            // keeps out of use.
            return Status::InvArgs;
    }

    auto self = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(kind, base, size, flags, name));
    if (!self) {
        return Status::OutOfMem;
    }

    rights = default_right();
    *out = KernelHandle<Self>(ustl::move(self));
    return Status::Ok;
}

auto ResourceDispatcher::is_root() const -> bool {
    return space_of(m_kind) != 0 && m_base == 0 && m_size == 0;
}

/// Every field is set at construction and never changes, so this takes no
/// lock.
auto ResourceDispatcher::get_info() const -> InfoResource {
    m_canary.verify<Self>();
    InfoResource info{};
    info.kind = m_kind;
    info.flags = m_flags;
    info.base = m_base;
    info.size = m_size;
    m_name.get(info.name, sizeof(info.name));
    return info;
}

auto validate_ranged_resource(ResourceDispatcher const &resource, RsrcKind const kind,
                              u64 const base, u64 const size) -> Status {
    if (resource.kind() != kind) {
        return Status::BadType;
    }

    // [base, base + size) inside [rbase, rbase + rsize), written so that
    // nothing overflows: base is compared first, then the rest measured from
    // the resource's own base. A root's range is its kind's whole space.
    auto rbase = resource.base();
    auto rsize = resource.size();
    if (resource.is_root()) {
        rbase = 0;
        rsize = space_of(kind);
    }
    if (base < rbase || size > rsize || base - rbase > rsize - size) {
        return Status::OutOfRange;
    }

    // Checked for every IoPort resource, the root included, since the root
    // is the one that covers these ports.
    if (kind == RsrcKind::IoPort && touches_kernel_ioports(base, size)) {
        return Status::AccessDenied;
    }
    return Status::Ok;
}

auto validate_ranged_resource(ours::Uandle const handle, RsrcKind const kind, u64 const base,
                              u64 const size) -> Status {
    ustl::Arc<ResourceDispatcher> resource;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::None, &resource);
        Status::Ok != status) {
        return status;
    }
    return validate_ranged_resource(*resource, kind, base, size);
}
} // namespace ours::object
