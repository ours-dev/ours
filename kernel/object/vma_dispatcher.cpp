/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <gktl/counter.hpp>
#include <ktl/new.hpp>
// The mapping entry points hand back an Arc<VmMapping>, and destroying an
// Arc needs the pointee's definition, not just its declaration -- vm_area.hpp
// only forward-declares VmMapping.
#include <ours/mm/vm_mapping.hpp>
#include <ours/object/vma_dispatcher.hpp>

GKTL_COUNTER(DISPATCHER_VMA_CREATE_COUNT, "dispatcher.vma.create");
GKTL_COUNTER(DISPATCHER_VMA_DESTROY_COUNT, "dispatcher.vma.destroy");

namespace ours::object {
VmaDispatcher::VmaDispatcher(ustl::Arc<mm::VmArea> vma) : Base(), m_vma(ustl::move(vma)) {
    DISPATCHER_VMA_CREATE_COUNT.add(1);
}

VmaDispatcher::~VmaDispatcher() {
    DISPATCHER_VMA_DESTROY_COUNT.add(1);
}

auto VmaDispatcher::create(ustl::Arc<mm::VmArea> vma, KernelHandle<Self> &handle_out,
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

    // TODO(SmallHuaZi): Zircon widens this from the VMAR's own CAN_MAP_READ /
    // WRITE / EXECUTE flags, so a region that cannot map executable pages
    // never hands out a handle claiming it can. ours' VmAreaOrMapping keeps
    // its VmaFlags private with no accessor, so the ceiling cannot be read
    // here yet; until it can, the default is the honest answer rather than a
    // guess at a narrower one.
    rights_out = default_right();
    handle_out = KernelHandle<Self>(ustl::move(self));
    return Status::Ok;
}

auto VmaDispatcher::map_phys(PhysAddr const phys_base, mm::VmPlacement const where,
                             usize const size, mm::MmuFlags const mmuf,
                             mm::VmMapOption const option, char const *const name)
    -> ktl::Result<ustl::Arc<mm::VmMapping>> {
    verify_canary();
    return m_vma->map_phys(phys_base, where, size, mmuf, option, name);
}

auto VmaDispatcher::map_anon(mm::VmPlacement const where, usize const size, mm::MmuFlags const mmuf,
                             mm::VmMapOption const option, char const *const name)
    -> ktl::Result<ustl::Arc<mm::VmMapping>> {
    verify_canary();
    return m_vma->map_anon(where, size, mmuf, option, name);
}

} // namespace ours::object
