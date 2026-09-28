/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/gaf.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/resource_dispatcher.hpp>

#include <heap/new.hpp>

namespace ours::object {
ResourceDispatcher::ResourceDispatcher(Kind kind, PhysAddr base, PhysAddr size, Flag flags,
                                       Name const &name)
    // Initializer order follows the member declaration order in the header.
    : Base()
    , m_canary(this)
    , m_name(name)
    , m_kind(kind)
    , m_flags(flags)
    , m_base(base)
    , m_size(size)
    , m_rsrclist(nullptr) {}

/// This used to check its arguments in a way no call could pass -- size 0 was
/// refused first, and then a Root resource was refused unless its size was 0
/// -- and returned Ok without ever constructing a dispatcher, leaving |out|
/// empty for a caller told it had succeeded. It now builds one, for the only
/// kind anything asks for.
auto ResourceDispatcher::create(KernelHandle<Self> *out, Rights &rights, Kind kind, PhysAddr base,
                                PhysAddr size, Flag flags, Name const &name, Storage *storage)
    -> Status {
    static_cast<void>(storage);
    if (!out) {
        return Status::InvArgs;
    }
    if (kind != Kind::System) {
        return Status::NotSupported;
    }

    // One slot at a time, and only a slot this kernel knows. Zircon checks the
    // same two things for ZX_RSRC_KIND_SYSTEM: a size of exactly 1 and a base
    // inside the slot table.
    if (size != 1 || base >= RSRC_SYSTEM_COUNT) {
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

auto validate_ranged_resource(HandleValue const handle, RsrcKind const kind, u64 const base,
                              u64 const size) -> Status {
    ustl::Arc<ResourceDispatcher> resource;
    if (auto const status = ProcessDispatcher::current()->handle_table().get_dispatcher(
            handle, Rights::None, &resource);
        Status::Ok != status) {
        return status;
    }
    if (resource->kind() != kind) {
        return Status::BadType;
    }

    // [base, base + size) inside [rbase, rbase + rsize), written so that
    // nothing overflows: base is compared first, then the rest measured from
    // the resource's own base.
    auto const rbase = u64(resource->base());
    auto const rsize = u64(resource->size());
    if (base < rbase || size > rsize || base - rbase > rsize - size) {
        return Status::OutOfRange;
    }
    return Status::Ok;
}
} // namespace ours::object
