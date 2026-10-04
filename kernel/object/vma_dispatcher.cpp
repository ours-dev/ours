/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <gktl/counter.hpp>
#include <heap/new.hpp>
// The mapping entry points hand back an Arc<VmMapping>, and destroying an
// Arc needs the pointee's definition, not just its declaration -- vm_area.hpp
// only forward-declares VmMapping.
#include <ours/mm/vm_mapping.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>

#include <ustl/mem/align.hpp>

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

    // Read, Write and Execute from what the region itself allows, as Zircon's
    // VmAddressRegionDispatcher::Create() adds them from the VMAR's own
    // CAN_MAP_READ / WRITE / EXECUTE: a region that cannot map executable
    // pages never hands out a handle claiming it can, and one that can -- a
    // process's root region -- hands out a handle that lets vma_map ask.
    auto rights = default_right();
    auto const allowed = self->m_vma->permissions();
    if (!!(allowed & mm::VmaFlags::Read)) {
        rights |= Rights::Read;
    }
    if (!!(allowed & mm::VmaFlags::Write)) {
        rights |= Rights::Write;
    }
    if (!!(allowed & mm::VmaFlags::Exec)) {
        rights |= Rights::Execute;
    }
    rights_out = rights;
    handle_out = KernelHandle<Self>(ustl::move(self));
    return Status::Ok;
}

auto VmaDispatcher::map(VmOptions const options, usize const vma_offset, VmoDispatcher &vmo,
                        u64 const vmo_offset, usize const length, char const *const name)
    -> ktl::Result<VirtAddr> {
    verify_canary();
    vmo.verify_canary();

    constexpr auto HANDLED = VmOptions::PermRead | VmOptions::PermWrite | VmOptions::PermExecute |
                             VmOptions::Specific | VmOptions::CanMapRead | VmOptions::CanMapWrite |
                             VmOptions::CanMapExecute | VmOptions::MapRange | VmOptions::Sealed;
    if ((options & ~HANDLED) != VmOptions::None) {
        return ktl::err(Status::NotSupported);
    }

    if (length == 0 || !ustl::mm::is_aligned(vmo_offset, PAGE_SIZE)) {
        return ktl::err(Status::InvArgs);
    }

    // W^X, for every caller (design D3): no mapping in any address space is
    // both writable and executable. vma_map refuses a program's request for
    // one before it gets here; this holds the kernel's own callers to the
    // same rule -- its loader maps every image by segment
    // (elfload::mapping_pages()), stacks are read and write, the vDSO's code
    // read and execute -- so that none can start to ask for one unnoticed.
    // Only the protection asked for now is checked: a ceiling may allow both
    // (CanMapWrite and CanMapExecute), and vma_protect, when it lands, has to
    // refuse raising a mapping to both. Zircon allows such a mapping and
    // leaves W^X to policy; here it is the system's rule.
    if (!!(options & VmOptions::PermWrite) && !!(options & VmOptions::PermExecute)) {
        return ktl::err(Status::InvArgs);
    }

    auto const rounded = ustl::mm::align_up(length, PAGE_SIZE);
    if (rounded == 0) {
        // Rounding wrapped: no VMO is that big, and no region either.
        return ktl::err(Status::OutOfRange);
    }

    // Each Perm bit needs its CanMap bit: a mapping may not start above its
    // own ceiling. Zircon answers ZX_ERR_ACCESS_DENIED the same way, from
    // VmAddressRegion::CreateVmMapping().
    auto const asks = [options](VmOptions perm, VmOptions can) {
        return !!(options & perm) && !(options & can);
    };
    if (asks(VmOptions::PermRead, VmOptions::CanMapRead) ||
        asks(VmOptions::PermWrite, VmOptions::CanMapWrite) ||
        asks(VmOptions::PermExecute, VmOptions::CanMapExecute)) {
        return ktl::err(Status::AccessDenied);
    }

    // Inside the VMO, written so that it cannot wrap.
    auto const vmo_size = u64(vmo.vmo()->size());
    if (rounded > vmo_size || vmo_offset > vmo_size - rounded) {
        return ktl::err(Status::OutOfRange);
    }

    // The vDSO's VMO is the one object whose mapping the kernel takes part in:
    // where its code lands becomes the address space's vDSO range, the only
    // place a `syscall` is accepted from. So its code is mapped executable only
    // in the shape that makes that range worth trusting, and once, and nothing
    // else of it is mapped executable at all.
    //
    // An executable mapping is the registration, and must be:
    //   - Exactly the code segment (VmoDispatcher::vdso_code()): the range
    //     recorded is then the vDSO's code and nothing else. The rest of the
    //     image -- the ELF header, the symbol and hash tables -- is data, and
    //     data that is executable inside the one trusted range is a place a
    //     jump can find the bytes of a `syscall` (0F 05) that no stub put
    //     there. It used to be mapped whole, all of it executable and all of
    //     it trusted.
    //   - Sealed. Registered, the range has to stay what it was registered
    //     as: sealed, nothing can unmap it or change its protection, so the
    //     real vDSO cannot be swapped for a program's own code containing a
    //     `syscall`. The launcher asks for it, as for any mapping it wants
    //     immutable, and the kernel refuses the vDSO without it -- so a
    //     launcher that forgot fails at the first start, loudly, instead of
    //     leaving a hole nobody sees.
    //   - Read and execute, nothing else.
    //   - Once per address space (claim_vdso(), taken just before the mapping):
    //     the range is fixed after it is set, so a second mapping cannot move
    //     it to somewhere the first is not.
    // Any other mapping of it is read-only, anywhere in it, as often as
    // wanted, and registers nothing: it is how the rest of the image is
    // placed beside the code. Never writable: there is no handle that lets a
    // program write these pages, but the rule does not lean on that -- it
    // holds for whoever holds the VMO.
    //
    // What refuses the unmap or protect is mm::VmArea (VmaFlags::Sealed);
    // vma_unmap reaches it, and vma_protect will when it lands.
    //
    // Zircon: VmAddressRegion::CreateSubVmarInner() lets an executable
    // mapping of the vDSO through only if VDso::valid_code_mapping() and the
    // address space has none yet, and records it as vdso_code_mapping_.
    auto const registers_vdso = vmo.is_vdso() && !!(options & VmOptions::PermExecute);
    if (vmo.is_vdso()) {
        constexpr auto PERMS = VmOptions::PermRead | VmOptions::PermWrite | VmOptions::PermExecute;
        constexpr auto CODE = VmOptions::PermRead | VmOptions::PermExecute;
        auto const code = vmo.vdso_code();
        if (!!(options & VmOptions::PermWrite)) {
            return ktl::err(Status::InvArgs);
        }
        if (registers_vdso && (vmo_offset != code.offset || rounded != code.size ||
                               !(options & VmOptions::Sealed) || (options & PERMS) != CODE)) {
            return ktl::err(Status::InvArgs);
        }
    }

    // Where: vma_offset counts from this region's start, and only means
    // something with Specific -- without it the kernel picks, and an offset
    // would be a request nobody reads. Zircon refuses that as well. Whether
    // the place lies inside this region is mm's to answer, after whether this
    // region takes a chosen place at all (CanMapSpecific): Zircon's order, so
    // a Specific request into a region without it is AccessDenied however far
    // off it points. mm's check is in offset form, so an offset that wraps the
    // address is refused there too.
    auto where = mm::VmPlacement::anywhere();
    if (!!(options & VmOptions::Specific)) {
        if (!ustl::mm::is_aligned(vma_offset, PAGE_SIZE)) {
            return ktl::err(Status::InvArgs);
        }
        where = mm::VmPlacement::at(m_vma->base() + vma_offset);
    } else if (vma_offset != 0) {
        return ktl::err(Status::InvArgs);
    }

    // The options in mm's words. User always: everything mapped through a
    // VMA handle is for ring 3.
    auto mmuf = mm::MmuFlags::User;
    auto ceiling = mm::VmaFlags::None;
    if (!!(options & VmOptions::PermRead)) {
        mmuf |= mm::MmuFlags::Readable;
    }
    if (!!(options & VmOptions::PermWrite)) {
        mmuf |= mm::MmuFlags::Writable;
    }
    if (!!(options & VmOptions::PermExecute)) {
        mmuf |= mm::MmuFlags::Executable;
    }
    if (!!(options & VmOptions::CanMapRead)) {
        ceiling |= mm::VmaFlags::Read;
    }
    if (!!(options & VmOptions::CanMapWrite)) {
        ceiling |= mm::VmaFlags::Write;
    }
    if (!!(options & VmOptions::CanMapExecute)) {
        ceiling |= mm::VmaFlags::Exec;
    }
    auto populate =
        !!(options & VmOptions::MapRange) ? mm::VmMapOption::Populate : mm::VmMapOption::None;
    if (!!(options & VmOptions::Sealed)) {
        populate |= mm::VmMapOption::Sealed;
    }

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
    //
    // The claim on the vDSO is taken last, after everything that can refuse
    // for a reason of its own, and given back if the mapping fails: a claim
    // held by a call that returned an error would keep the vDSO from ever
    // being mapped.
    if (registers_vdso && !m_vma->aspace()->claim_vdso()) {
        return ktl::err(Status::AccessDenied);
    }
    auto mapping =
        m_vma->map_with_vmo(where, rounded, vmo_offset, mmuf, ceiling, vmo.vmo(), populate, name);
    if (mapping.is_err()) {
        if (registers_vdso) {
            m_vma->aspace()->release_vdso();
        }
        return ktl::err(mapping.unwrap_err());
    }
    auto const base = mapping.unwrap()->base();
    if (registers_vdso) {
        m_vma->aspace()->set_vdso(ustl::move(mapping.unwrap()));
    }
    return ktl::ok(base);
}

auto VmaDispatcher::allocate(VmOptions const options, usize const offset, usize const size,
                             KernelHandle<Self> &child_out, Rights &rights_out, VirtAddr &base_out)
    -> Status {
    verify_canary();

    // A region has no protection of its own, only what it allows mappings in
    // it to have, so a Perm bit means nothing here. Zircon refuses it the same
    // way (VmAddressRegionDispatcher::Allocate(), "MMU-related flags").
    constexpr auto PERM = VmOptions::PermRead | VmOptions::PermWrite | VmOptions::PermExecute;
    if ((options & PERM) != VmOptions::None) {
        return Status::InvArgs;
    }
    // Anything else not acted on yet: Compact, OffsetIsUpperLimit,
    // SpecificOverwrite and the mapping-only bits.
    constexpr auto CAN_MAP_RWX =
        VmOptions::CanMapRead | VmOptions::CanMapWrite | VmOptions::CanMapExecute;
    constexpr auto ACTED_ON = VmOptions::Specific | CAN_MAP_RWX | VmOptions::CanMapSpecific;
    if ((options & ~ACTED_ON) != VmOptions::None) {
        return Status::NotSupported;
    }

    if (size == 0 || !ustl::mm::is_aligned(offset, PAGE_SIZE)) {
        return Status::InvArgs;
    }
    auto const rounded = ustl::mm::align_up(size, PAGE_SIZE);
    if (rounded == 0) {
        return Status::OutOfRange;
    }

    // Where: |offset| counts from this region's start with Specific; without
    // it the kernel finds room, and an offset would be a request nobody reads,
    // so it must be 0 (Zircon: VmAddressRegion::CreateSubVmarInner()). As in
    // map(), whether the place lies inside this region is mm's to answer,
    // after CanMapSpecific.
    auto where = mm::VmPlacement::anywhere();
    if (!!(options & VmOptions::Specific)) {
        where = mm::VmPlacement::at(m_vma->base() + offset);
    } else if (offset != 0) {
        return Status::InvArgs;
    }

    // What the sub-region allows, from the CanMap bits. With none of them it
    // allows nothing, and every mapping asked of it fails -- a guard. It may
    // narrow this region's allowance and never widen it: AccessDenied, as
    // Zircon's "vmar_flags & ~flags_ & VMAR_CAN_RWX_FLAGS". The system call has
    // already asked the handle for the matching rights; this is the region's
    // own word, for a caller that has no handle to ask.
    auto allows = mm::VmaFlags::None;
    if (!!(options & VmOptions::CanMapRead)) {
        allows |= mm::VmaFlags::Read;
    }
    if (!!(options & VmOptions::CanMapWrite)) {
        allows |= mm::VmaFlags::Write;
    }
    if (!!(options & VmOptions::CanMapExecute)) {
        allows |= mm::VmaFlags::Exec;
    }
    constexpr auto RWX = mm::VmaFlags::Read | mm::VmaFlags::Write | mm::VmaFlags::Exec;
    if ((allows & RWX & ~m_vma->permissions()) != mm::VmaFlags::None) {
        return Status::AccessDenied;
    }
    // Places chosen inside the sub-region: no right asked, and no ceiling
    // from this region, whose own CanMapSpecific decides only whether the
    // sub-region itself can go at |offset| (mm refuses that below). Without
    // it the sub-region lands where the kernel puts it, so places chosen
    // inside it fix only distances. Zircon: "It is NOT an error if the parent
    // does not have ZX_VM_CAN_MAP_SPECIFIC permissions" (zx_vmar_allocate).
    if (!!(options & VmOptions::CanMapSpecific)) {
        allows |= mm::VmaFlags::CanMapSpecific;
    }

    // mm places it, and refuses a range that is taken or runs off this region.
    ustl::Arc<mm::VmArea> child;
    if (auto const status = m_vma->create_subvma(where, rounded, allows, "vma_allocate",
                                                 mm::VmMapOption::None, &child);
        Status::Ok != status) {
        return status;
    }
    base_out = child->base();
    return create(ustl::move(child), child_out, rights_out);
}

auto VmaDispatcher::op_children_from_rights(Rights const rights) -> OpChildren {
    if (!!(rights & Rights::OpChildren)) {
        return OpChildren::Yes;
    }
    return OpChildren::No;
}

auto VmaDispatcher::unmap(VirtAddr const base, usize const length, OpChildren const op_children)
    -> Status {
    verify_canary();

    // mm rounds an unaligned base down, which suits a kernel caller that names
    // the page an address lies in. A program that passes one has made a
    // mistake, and rounding would unmap bytes before the ones it named.
    if (!ustl::mm::is_aligned(base, PAGE_SIZE)) {
        return Status::InvArgs;
    }
    return m_vma->unmap(base, length, op_children);
}

} // namespace ours::object
