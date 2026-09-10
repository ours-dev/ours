/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_VMA_DISPATCHER_HPP
#define OURS_OBJECT_VMA_DISPATCHER_HPP 1

#include <ours/mm/vm_area.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>

#include <ktl/result.hpp>

namespace ours::object {
/// Rights a freshly created VMA handle carries.
constexpr auto RIGHTS_DEFAULT_VMA = Rights::Signal | Rights::Io;

using VmaDispatcherBase = SoloDispatcher<VmaDispatcher, RIGHTS_DEFAULT_VMA, Signals::None>;

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
/// know who is asking. Every future sys_vmar_map has to answer "may this
/// caller map here", and the only object in the tree that can answer it is the
/// handle that got the caller here -- which means the mapping entry point has
/// to be on the dispatcher, not on the VmArea.
/// `final`: a dispatcher is a leaf. Nothing derives from one -- the type
/// hierarchy stops at Dispatcher -> SoloDispatcher -> the concrete object --
/// and saying so is what lets the destructor below be marked `final` too.
/// Zircon spells VmAddressRegionDispatcher the same way.
class VmaDispatcher final: public VmaDispatcherBase {
    using Self = VmaDispatcher;
    using Base = VmaDispatcherBase;

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

    /// Map |size| bytes of |vmo| where |where| says.
    ///
    /// One entry point taking a VMO, where there used to be two -- map_phys()
    /// for a physical range and map_anon() for fresh anonymous pages. Those
    /// were mm::VmArea's convenience wrappers surfaced verbatim, and both end
    /// in the same mapping call; what differed was only which kind of
    /// VmObject they built on the caller's behalf. Building it is now the
    /// caller's own step (VmoDispatcher::create_paged / create_physical),
    /// which is what makes "get some memory" and "put it somewhere" two
    /// operations rather than one -- the split Zircon has between
    /// zx_vmo_create and zx_vmar_map, and the reason the same VMO can be
    /// mapped twice at two addresses.
    ///
    /// No VMO offset, unlike zx_vmar_map: the whole VMO is mapped from its
    /// start. mm::VmArea::map_with_vmo() hardcodes the offset to zero when it
    /// calls create_mapping(), so an offset here could only be accepted and
    /// ignored -- and this file has just been bitten once by an argument that
    /// was accepted and ignored (see map_with_vmo's own note about Commit).
    /// The parameter arrives when mm can honour it.
    ///
    /// Still no rights check: there is no handle-holding caller to check
    /// against yet. It belongs at the syscall entry once handle resolution
    /// exists -- and it belongs on this side of the call, which is why the
    /// entry point is here rather than on mm::VmArea.
    auto map(VmoDispatcher &vmo, mm::VmPlacement where, usize size, mm::MmuFlags mmuf,
             mm::VmMapOption option, char const *name) -> ktl::Result<ustl::Arc<mm::VmMapping>>;

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

    static constexpr auto MAGIC = gktl::magic("VmaDispatcher");

    explicit VmaDispatcher(ustl::Arc<mm::VmArea> vma);

    /// `final`, because Dispatcher's destructor is virtual: without it this
    /// silently overrides rather than saying so, and nothing would catch a
    /// later signature change that stopped overriding.
    ~VmaDispatcher() final;

    ustl::Arc<mm::VmArea> m_vma;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_VMA_DISPATCHER_HPP
