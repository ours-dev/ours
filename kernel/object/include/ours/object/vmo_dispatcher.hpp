/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_VMO_DISPATCHER_HPP
#define OURS_OBJECT_VMO_DISPATCHER_HPP 1

#include <ours/mm/vm_object.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>

namespace ours::object {
/// Rights a freshly created VMO handle carries.
///
/// This constant used to have nowhere to live. system/public/ours/rights.hpp
/// carried it as a comment -- per-object default rights are kernel policy
/// rather than public ABI, so each belongs beside its dispatcher, and there
/// was no VMO dispatcher header to put it beside. There is now.
/// Zircon ZX_DEFAULT_VMO_RIGHTS. Execute is not among them: mapping a VMO
/// executable takes a handle that was granted it separately.
constexpr auto RIGHTS_DEFAULT_VMO =
    Rights::Basic | Rights::Io | Rights::Property | Rights::Map | Rights::Signal;

using VmoDispatcherBase = SoloDispatcher<VmoDispatcher, RIGHTS_DEFAULT_VMO, Signals::None>;

/// The object-layer face of one mm::VmObject: a range of memory that exists
/// independently of any address space, and that a VmaDispatcher can map.
///
/// Zircon counterpart: VmObjectDispatcher.
///
/// The split between this and VmaDispatcher is the whole shape of the VM ABI:
/// a VMO is *what* the memory is, a VMA is *where* it appears. Keeping them
/// apart is what lets the same pages be mapped twice, at different addresses
/// with different protections, and what makes "hand a program some memory"
/// (pass a VMO handle) a different operation from "put it in that program's
/// address space" (map it into its VMA).
class VmoDispatcher final: public VmoDispatcherBase {
    using Self = VmoDispatcher;
    using Base = VmoDispatcherBase;

  public:
    /// Wraps an already-built VmObject. Zircon's VmObjectDispatcher::Create
    /// has exactly this shape, and for the same reason: what kind of VmObject
    /// to build is the caller's decision, not this layer's.
    static auto create(ustl::Arc<mm::VmObject> vmo, KernelHandle<Self> &handle_out,
                       Rights &rights_out) -> Status;

    /// The two kinds of VMO this kernel can currently build, named here so
    /// that a caller in the object layer never has to reach down and spell
    /// mm::VmObjectPaged or mm::VmObjectPhysical itself.
    ///
    /// This is a deliberate departure from Zircon, where sys_vmo_create builds
    /// the VmObject and only then wraps it: there, the syscall layer is
    /// already inside the VM subsystem's own headers. Here the rule is that
    /// the layer above uses kernel/object and nothing below it, so the object
    /// layer owes it a way to say what it wants without naming mm's types.

    /// Anonymous, zero-filled memory -- the backing for a stack, a heap, a
    /// .bss. `size` is rounded up to a page by the VMO itself.
    static auto create_paged(usize size, KernelHandle<Self> &handle_out, Rights &rights_out)
        -> Status;

    /// A window onto physical memory that already exists and that this object
    /// does not own: MMIO, or -- as userboot's loaded image currently is --
    /// frames some other allocation is keeping alive. Nothing here frees them.
    static auto create_physical(PhysAddr base, usize size, KernelHandle<Self> &handle_out,
                                Rights &rights_out) -> Status;

    FORCE_INLINE
    auto vmo() const -> ustl::Arc<mm::VmObject> const & {
        return m_vmo;
    }

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the
    // canary; MAGIC stays private otherwise, so it needs this base in.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    /// RefCounted::destroy() calls ~Self() when the last reference goes, so
    /// the destructor is reachable from the base rather than from callers.
    friend class ustl::RefCounted<Dispatcher, ustl::sync::ThreadSafePolicy>;

    static constexpr auto MAGIC = gktl::magic("VmoDispatcher");

    explicit VmoDispatcher(ustl::Arc<mm::VmObject> vmo);
    ~VmoDispatcher() final;

    ustl::Arc<mm::VmObject> m_vmo;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_VMO_DISPATCHER_HPP
