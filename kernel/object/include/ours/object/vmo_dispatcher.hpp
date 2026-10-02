/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_VMO_DISPATCHER_HPP
#define OURS_OBJECT_VMO_DISPATCHER_HPP 1

#include <ours/mm/vm_object.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/syscall.hpp>

#include <user_copy/user_copy.hpp>

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
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Vmo);

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

    /// Anonymous memory with every page committed when it is made, zeroed, and
    /// not pinned -- for data the kernel fills before user space sees it, such
    /// as the BootFS.
    ///
    /// Not pinned, because a pinned VMO cannot be snapshotted
    /// (mm::VmObjectPaged::create_clone() answers BadState: a clone moves the
    /// pages into a hidden node and a later write forks them, so they would
    /// not stay where a pin says they are), and a program's data segment is a
    /// snapshot of its file in the BootFS. Committed, so that the bytes the
    /// kernel writes need no fault and the gaps it leaves read as zero.
    ///
    /// Zircon's BootFS VMO is likewise an ordinary paged VMO, never pinned:
    /// userboot makes it with zx_vmo_create and writes every page of it.
    static auto create_committed(usize size, KernelHandle<Self> &handle_out, Rights &rights_out)
        -> Status;

    /// The VMO a vmo_create call makes: |size| bytes of anonymous memory,
    /// committed lazily -- a page exists only once it is touched.
    ///
    ///   - InvArgs for an option bit that means nothing; NotSupported for
    ///     Resizable, which is known and deferred.
    ///   - OutOfRange when |size| is past mm::VmObjectPaged::MAX_SIZE.
    ///   - |size| is rounded up to a page; zero is a valid, empty VMO.
    ///   - The handle's rights are the default VMO rights, which have no
    ///     Execute (D3).
    ///
    /// Unlike create_paged(), which the kernel uses for memory that must be
    /// resident before ring 3 runs, nothing here is committed up front: a
    /// program may ask for a VMO far larger than it will ever touch.
    ///
    /// Zircon: sys_vmo_create() with VmObjectDispatcher::
    /// parse_create_syscall_flags(), which checks the options and the size the
    /// same way before VmObjectPaged::Create().
    static auto create_user(u64 size, VmoOptions options, KernelHandle<Self> &handle_out,
                            Rights &rights_out) -> Status;

    /// A child of this VMO, for vmo_create_child; see the definition. The
    /// rights are the caller's to decide -- they come from the parent's
    /// handle, which this object never sees.
    auto create_child(VmoChildOptions options, u64 offset, u64 size, KernelHandle<Self> &handle_out)
        -> Status;

    /// Whether |options| asks for a snapshot, of any kind, and nothing else.
    /// The one list of them: create_child() makes a clone for exactly these,
    /// and vmo_create_child() gives exactly these Write and takes Execute
    /// away, so a kind added here is added to both.
    static constexpr auto is_snapshot(VmoChildOptions const options) -> bool {
        return options == VmoChildOptions::SnapshotFull ||
               options == VmoChildOptions::SnapshotOnWrite ||
               options == VmoChildOptions::SnapshotModified;
    }

    /// A window onto physical memory that already exists and that this object
    /// does not own, such as MMIO. Nothing here frees it. (Loaded ELF images
    /// used to come through here too, which is how their frames leaked; they
    /// are paged VMOs now.)
    static auto create_physical(PhysAddr base, usize size, KernelHandle<Self> &handle_out,
                                Rights &rights_out) -> Status;

    FORCE_INLINE
    auto vmo() const -> ustl::Arc<mm::VmObject> const & {
        return m_vmo;
    }

    /// Marks this as the vDSO's VMO, the one kernel object every process's
    /// system calls go through. Done once, by whoever lays the vDSO out, before
    /// any process exists; nothing unmarks it.
    ///
    /// What the mark changes: a mapping of it is the registration of the
    /// address space's vDSO range (VmaDispatcher::map()), and is allowed only
    /// in the one shape that makes the range trustworthy. It is a property of
    /// this object and not of a handle to it, so every handle, duplicate and
    /// holder is subject to it alike; a child of it (vmo_create_child) is
    /// another object and is not the vDSO.
    ///
    /// Zircon: the VDso class, which owns the vDSO's VMOs and is what the
    /// vDSO mapping is recognised by.
    auto mark_vdso() -> void {
        m_is_vdso = true;
    }

    [[nodiscard]]
    FORCE_INLINE
    auto is_vdso() const -> bool {
        return m_is_vdso;
    }

    /// Copies |size| bytes starting at byte |offset| of the VMO into the
    /// kernel buffer |out|. A range not wholly inside the VMO is OutOfRange
    /// and copies nothing; a kind of VMO that cannot be read this way (a
    /// physical one) is NotSupported.
    ///
    /// For the kernel's own use (userabi fills and checks the BootFS VMO
    /// through this and write()). The copy runs under the VMO's lock
    /// (mm::VmObject::read()) and must not fault, which is why it takes a
    /// kernel buffer; a user buffer goes through read_user(). No rights are
    /// checked here -- that is the caller's, against its handle.
    auto read(void *out, u64 offset, usize size) -> Status {
        return m_vmo->read(out, VirtAddr(offset), size);
    }

    /// Copies [offset, offset + length) of the VMO to the user buffer |out|:
    /// the body of the vmo_read system call. Zircon
    /// VmObjectDispatcher::Read(), which takes a user pointer the same way.
    ///
    ///   - OutOfRange when the range does not lie wholly inside the VMO,
    ///     checked before anything is read, so nothing is delivered. A
    ///     zero-length range is in range up to and including the end.
    ///   - Zero length is then Ok without looking at |out|.
    ///   - InvArgs when |out| cannot take |length| bytes: outside the user
    ///     half (checked before the first page is read), or unmapped or
    ///     read-only somewhere along it (found while copying, and what was
    ///     copied before that stays there).
    ///   - NotSupported when the VMO cannot be read at all (a physical one),
    ///     but only once the checks above have passed; see the definition.
    ///   - All or nothing on success; no count on failure.
    ///
    /// No rights are checked here; the system call has checked Read.
    auto read_user(user_copy::UserOutPtr<u8> out, u64 offset, usize length) -> Status;

    /// Copies |size| bytes from the kernel buffer |in| into the VMO at byte
    /// |offset|. The same rules as read(); Zircon VmObjectDispatcher::Write().
    auto write(void const *in, u64 offset, usize size) -> Status {
        return m_vmo->write(in, VirtAddr(offset), size);
    }

    /// Copies |length| bytes from the user buffer |in| into the VMO at byte
    /// |offset|: the body of the vmo_write system call, read_user()'s mirror.
    /// Zircon VmObjectDispatcher::Write(), which takes a user pointer the same
    /// way.
    ///
    ///   - OutOfRange when the range does not lie wholly inside the VMO,
    ///     checked before anything is written.
    ///   - Zero length is then Ok without looking at |in|.
    ///   - InvArgs when |in| cannot give |length| bytes: outside the user half
    ///     (checked before the first page is touched), or unmapped somewhere
    ///     along it (found while copying). The VMO bytes before that point may
    ///     have been written by then.
    ///   - NotSupported when the VMO cannot be written this way (a physical
    ///     one), once the checks above have passed.
    ///
    /// No rights are checked here; the system call has checked Write.
    auto write_user(user_copy::UserInPtr<u8 const> in, u64 offset, usize length) -> Status;

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

    /// See mark_vdso(). Written before the object is shared, read after: no
    /// synchronisation needed.
    bool m_is_vdso = false;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_VMO_DISPATCHER_HPP
