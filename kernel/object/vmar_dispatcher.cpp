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

auto VmarDispatcher::map(VmoDispatcher &vmo, mm::VmPlacement const where, usize const size,
                         mm::MmuFlags const mmuf, mm::VmMapOption const option,
                         char const *const name) -> ktl::Result<ustl::Arc<mm::VmMapping>> {
    verify_canary();
    vmo.verify_canary();

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
    return m_vma->map_with_vmo(where, size, mmuf, vmo.vmo(), option, name);
}

} // namespace ours::object
