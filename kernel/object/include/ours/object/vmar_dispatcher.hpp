/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_VMAR_DISPATCHER_HPP
#define OURS_OBJECT_VMAR_DISPATCHER_HPP 1

#include <ours/mm/vm_area.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/syscall.hpp>

#include <ktl/result.hpp>

namespace ours::object {
/// Rights a freshly created VMA handle carries.
/// Zircon ZX_DEFAULT_VMAR_RIGHTS: a region is not waited on, and what may be
/// mapped into it is added per handle (Read/Write/Execute) by whoever creates
/// it, not granted by default.
constexpr auto RIGHTS_DEFAULT_VMAR = (Rights::Basic & ~Rights::Wait) | Rights::OpChildren;

using VmarDispatcherBase = SoloDispatcher<VmarDispatcher, RIGHTS_DEFAULT_VMAR, Signals::None>;

/// The object-layer face of one mm::VmArea.
///
/// Zircon counterpart: VmAddressRegionDispatcher, which wraps a
/// VmAddressRegion the same way.
///
/// It *holds* a VmArea rather than being one. That is not only the project's
/// standing preference for composition: the two have different lifetimes and
/// different owners. A VmArea is owned by its address space and outlives every
/// handle to it; the dispatcher is owned by whoever holds a handle and dies
/// when the last one closes. Deriving would fuse those two lifetimes into one
/// and make "the process closed its VMA handle" mean "the address space lost
/// a region", which is not what closing a handle means.
///
/// The reason this type has to exist at all, rather than letting bring-up code
/// map through mm::VmArea directly, is that rights live on handles. A VmArea
/// knows what a mapping is *allowed* to be (its VmaFlags ceiling); it does not
/// know who is asking. Every future syscall::vmar_map has to answer "may this
/// caller map here", and the only object in the tree that can answer it is the
/// handle that got the caller here -- which means the mapping entry point has
/// to be on the dispatcher, not on the VmArea.
/// `final`: a dispatcher is a leaf. Nothing derives from one -- the type
/// hierarchy stops at Dispatcher -> SoloDispatcher -> the concrete object --
/// and saying so is what lets the destructor below be marked `final` too.
/// Zircon spells VmAddressRegionDispatcher the same way.
class VmarDispatcher final: public VmarDispatcherBase {
    using Self = VmarDispatcher;
    using Base = VmarDispatcherBase;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Vmar);

    /// Wraps |vma| in a new dispatcher and reports the rights its first handle
    /// should carry.
    ///
    /// The rights come out as a separate `Rights &` rather than being read off
    /// the returned object because that is where they live: a Handle carries
    /// rights, a Dispatcher does not, so the only thing create() can do is
    /// hand the caller the value to build the Handle with. Zircon's
    /// VmAddressRegionDispatcher::Create has the same two out-parameters for
    /// the same reason.
    static auto create(ustl::Arc<mm::VmArea> vma, KernelHandle<Self> &handle_out,
                       Rights &rights_out) -> Status;

    /// Maps [vmo_offset, vmo_offset + length) of |vmo| into this region, and
    /// answers the address it landed at.
    ///
    /// One entry point taking a VMO, where there used to be two -- map_phys()
    /// for a physical range and map_anon() for fresh anonymous pages. Building
    /// the VMO is the caller's own step (VmoDispatcher::create_*), which is
    /// what makes "get some memory" and "put it somewhere" two operations --
    /// the split Zircon has between zx_vmo_create and zx_vmar_map, and the
    /// reason the same VMO can be mapped twice at two addresses.
    ///
    /// It used to take mm's placement and MMU flags and no VMO offset (mm
    /// hardcoded it to zero), so the object layer's interface was mm's.
    ///
    /// Zircon's shape: VmAddressRegionDispatcher::Map() takes the options
    /// word and no rights. The rights are the system call's to turn into
    /// options -- a Perm bit the handles do not allow is refused there, and
    /// the CanMap bits it sets from them become the mapping's ceiling here --
    /// so a kernel caller, which has no handle to ask, says what it means in
    /// the same words. Interface design 2.3 had this take the two handles'
    /// rights instead; the options word says the same and keeps one meaning
    /// for CanMap from the kernel up to vmar_allocate.
    ///
    /// Only Perm*, Specific, CanMap* and MapRange are acted on; any other bit
    /// is NotSupported. Otherwise:
    ///   - InvArgs: length 0; vmo_offset not page-aligned; vmar_offset
    ///     without Specific, or not page-aligned, or outside this region;
    ///   - AccessDenied: a Perm bit without its CanMap bit;
    ///   - OutOfRange: the range, rounded up to a page, not inside the VMO.
    ///     Zircon lets a mapping run past its VMO's end and faults the
    ///     access instead; interface design 2.3 refuses it at the door.
    /// W^X and "Write needs Read" are not checked here: they are rules for
    /// what a program may ask (vmar_map), and userboot still maps its image
    /// writable and executable as one range until segments get their own.
    ///
    /// |name| names the mapping in the kernel's dumps; the system call passes
    /// its own.
    auto map(VmOptions options, usize vmar_offset, VmoDispatcher &vmo, u64 vmo_offset, usize length,
             char const *name) -> ktl::Result<VirtAddr>;

    /// Reserves [offset, offset + size) of this region as a sub-region of its
    /// own, and hands back a dispatcher for it, its rights and its address.
    ///
    /// Only the one shape the launcher needs today, a guard: Specific, and no
    /// CanMap bit, so the sub-region can hold no mapping at all and nothing
    /// placed "anywhere" later can land in it (interface design 3.4). A Perm
    /// bit is InvArgs, as in Zircon -- a region has no protection of its own,
    /// only what it allows -- and anything else, CanMap* and a missing
    /// Specific included, is NotSupported until a caller needs it. Otherwise
    /// InvArgs for a size of 0, and for an offset that is not page-aligned or
    /// lies outside this region; |size| rounds up to a page.
    ///
    /// The sub-region belongs to this one, not to the handle: closing every
    /// handle to it leaves it in place, and it goes with the address space.
    /// Zircon: VmAddressRegionDispatcher::Allocate().
    auto allocate(VmOptions options, usize offset, usize size, KernelHandle<Self> &child_out,
                  Rights &rights_out, VirtAddr &base_out) -> Status;

    /// Tears down whatever is mapped in [base, base + length) of this region.
    /// Today vmar_map and vmar_allocate call it, to take back what they made
    /// when they could not report it; vmar_unmap will be the next caller. A
    /// sub-region wholly inside the range is destroyed, not left empty.
    auto unmap(VirtAddr base, usize length) -> Status {
        verify_canary();
        return m_vma->unmap(base, length);
    }

    FORCE_INLINE
    auto vma() const -> ustl::Arc<mm::VmArea> const & {
        return m_vma;
    }

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the
    // canary; MAGIC stays private otherwise, so it needs this base in.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    /// RefCounted::destroy() calls ~Self() when the last reference goes, so
    /// the destructor is reachable from the base rather than from callers.
    friend class ustl::RefCounted<Dispatcher, ustl::sync::ThreadSafePolicy>;

    static constexpr auto MAGIC = gktl::magic("VmarDispatcher");

    explicit VmarDispatcher(ustl::Arc<mm::VmArea> vma);

    /// `final`, because Dispatcher's destructor is virtual: without it this
    /// silently overrides rather than saying so, and nothing would catch a
    /// later signature change that stopped overriding.
    ~VmarDispatcher() final;

    ustl::Arc<mm::VmArea> m_vma;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_VMAR_DISPATCHER_HPP
