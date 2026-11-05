/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/alloc_flags.hpp>
#include <ours/object/iommu_dispatcher.hpp>

#include <heap/new.hpp>

namespace ours::object {
IommuDispatcher::IommuDispatcher(IommuType const type) : Base(), m_type(type) {}

auto IommuDispatcher::create(IommuType const type, usize const desc_size,
                             KernelHandle<Self> &handle_out, Rights &rights_out) -> Status {
    // A value outside the enum is a type this kernel has no code for, as much
    // as a named one it has not built yet.
    if (type != IommuType::Stub) {
        return Status::NotSupported;
    }
    if (desc_size != 0) {
        return Status::InvArgs;
    }

    auto self = ustl::Arc<Self>::adopt(new (mm::ALLOC_KERNEL) Self(type));
    if (!self) {
        return Status::NoMemory;
    }

    rights_out = default_right();
    handle_out = KernelHandle<Self>(ustl::move(self));
    return Status::Ok;
}
} // namespace ours::object
