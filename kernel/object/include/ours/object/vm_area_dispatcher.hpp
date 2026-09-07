/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_VM_AREA_DISPATCHER_HPP
#define OURS_OBJECT_VM_AREA_DISPATCHER_HPP 1

#include <ktl/result.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>

namespace ours::object {
/// Rights a freshly created VMAR handle carries.
constexpr auto RIGHTS_DEFAULT_VMAR = Rights::Signal | Rights::Io;

using VmAreaDispatcherBase = SoloDispatcher<VmAreaDispatcher, RIGHTS_DEFAULT_VMAR, Signals::None>;

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
/// and make "the process closed its VMAR handle" mean "the address space lost
/// a region", which is not what closing a handle means.
///
/// The reason this type has to exist at all, rather than letting bring-up code
/// map through mm::VmArea directly, is that rights live on handles. A VmArea
/// knows what a mapping is *allowed* to be (its VmaFlags ceiling); it does not
/// know who is asking. Every future sys_vmar_map has to answer "may this
/// caller map here", and the only object in the tree that can answer it is the
/// handle that got the caller here -- which means the mapping entry point has
/// to be on the dispatcher, not on the VmArea.
class VmAreaDispatcher: public VmAreaDispatcherBase {
    using Self = VmAreaDispatcher;
    using Base = VmAreaDispatcherBase;

  public:
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

    /// The two ways to get memory mapped into this area.
    ///
    /// They mirror mm::VmArea's own pair exactly, and deliberately add nothing
    /// on top of it yet -- there is no rights check here because there is no
    /// caller yet that has a Handle to check against; userboot is built by the
    /// kernel itself. The check belongs at the syscall entry, once a handle
    /// resolution step exists to produce the Rights.
    ///
    /// They will also collapse into a single map(VmObjectDispatcher, ...) once
    /// a VMO dispatcher exists: Zircon has only Map(), taking a VmObject,
    /// because in Zircon *everything* mappable is a VMO. Ours' VmArea still
    /// offers the "map this physical range" shortcut that userboot's image
    /// currently uses, so both are exposed rather than pretending the VMO
    /// layer is already here.
    auto map_phys(PhysAddr phys_base, mm::VmPlacement where, usize size, mm::MmuFlags mmuf,
                  mm::VmMapOption option, char const *name)
        -> ktl::Result<ustl::Arc<mm::VmMapping>>;

    auto map_anon(mm::VmPlacement where, usize size, mm::MmuFlags mmuf, mm::VmMapOption option,
                  char const *name) -> ktl::Result<ustl::Arc<mm::VmMapping>>;

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

    static constexpr auto MAGIC = gktl::magic("VmAreaDispatcher");

    explicit VmAreaDispatcher(ustl::Arc<mm::VmArea> vma);
    ~VmAreaDispatcher();

    ustl::Arc<mm::VmArea> m_vma;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_VM_AREA_DISPATCHER_HPP
