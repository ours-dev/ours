/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PAGER_DISPATCHER_HPP
#define OURS_OBJECT_PAGER_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/pager_proxy.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/syscall.hpp>

namespace ours::object {
class PagerDispatcher;
class VmoDispatcher;

/// Rights a freshly created pager handle carries.
constexpr auto RIGHTS_DEFAULT_PAGER =
    Rights::Inspect | Rights::Transfer | Rights::AttachVmo | Rights::ManageVmo;

using PagerDispatcherBase = SoloDispatcher<PagerDispatcher, RIGHTS_DEFAULT_PAGER>;

/// The object behind a pager handle: a program that supplies the pages of
/// the VMOs it makes, as a file system supplies a file's.
///
/// It owns the operations on its VMOs, so that the system-call layer never
/// names an mm type: create_vmo(), supply_pages(), detach_vmo(). Each VMO's
/// requests reach the pager through that VMO's PagerProxy, as packets on the
/// port it was made with.
///
/// The objects: m_proxies, the proxies of this pager's VMOs that are still
/// tied to it, each kept by its own m_pager_ref; and m_zero_handles, set
/// once the last handle is gone, after which no VMO is made. Both are
/// guarded by m_list_lock, a leaf: a proxy takes it under its own lock to
/// leave the list (release_proxy()), and the pager never calls a proxy while
/// holding it.
class PagerDispatcher final: public PagerDispatcherBase {
    using Self = PagerDispatcher;
    using Base = PagerDispatcherBase;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Pager);

    /// Makes a pager. |options| must be 0, InvArgs otherwise: none is
    /// defined yet.
    static auto create(u32 options, KernelHandle<Self> &handle_out, Rights &rights_out) -> Status;

    /// A VMO of |size| bytes, rounded up to a page, whose pages this pager
    /// supplies: its requests go to |port| under |key|.
    ///
    ///   - |options| None; Resizable is NotSupported, as vmo_create has it,
    ///     and any other bit InvArgs. Checked first.
    ///   - OutOfRange when |size| is past mm::VmObjectPaged::MAX_SIZE.
    ///   - BadState once the pager has lost its last handle, which a call
    ///     racing that close can see.
    ///   - The handle's rights are the default VMO rights.
    auto create_vmo(VmoOptions options, ustl::Arc<PortDispatcher> port, u64 key, u64 size,
                    KernelHandle<VmoDispatcher> &handle_out, Rights &rights_out) -> Status;

    /// Moves [aux_offset, aux_offset + length) of |aux|'s pages into
    /// [offset, offset + length) of |vmo|, which must be one of this pager's
    /// (InvArgs otherwise): mm::VmObjectPaged::take_pages() out of |aux|, then
    /// supply_pages() into |vmo|, one after the other, never both locked.
    /// Page aligned, InvArgs otherwise; zero length is Ok and does nothing.
    /// The errors of the two are the call's, as they come.
    auto supply_pages(VmoDispatcher &vmo, u64 offset, u64 length, VmoDispatcher &aux,
                      u64 aux_offset) -> Status;

    /// Detaches |vmo|, which must be one of this pager's (InvArgs otherwise),
    /// from this pager: mm::VmObject::detach_source(). Its port gets Complete.
    auto detach_vmo(VmoDispatcher &vmo) -> Status;

    /// No handle is left to supply anything: every VMO still tied to this
    /// pager is detached, and what its proxy still has on the port is taken
    /// back (PagerProxy::on_pager_close()).
    auto on_zero_handles() -> void override;

    enum class Operation : u8 {
        Dirty,
        WriteBackPreamble,
        WriteBackPostamble,
    };

    auto operate_range(Operation op, ustl::Arc<mm::VmObject> vmo, usize offset, usize len,
                       usize priv_data) -> Status;

  private:
    friend class PagerProxy;

    friend class ustl::RefCounted<Dispatcher, ustl::sync::ThreadSafePolicy>;

    PagerDispatcher() = default;
    ~PagerDispatcher() final;

    /// Whether |vmo| is one of this pager's: its source's owner id is this
    /// pager's koid.
    [[nodiscard]]
    auto owns(VmoDispatcher &vmo) const -> bool;

    /// Takes |proxy| off the list and hands back the reference the list kept,
    /// for the caller to drop once it has let go of its locks; empty if it is
    /// off already (the pager is closing and took it). Called with |proxy|'s
    /// lock held.
    auto release_proxy(PagerProxy *proxy) -> ustl::Arc<PagerProxy>;

    sync::Mutex m_list_lock;
    PagerProxy::PagerList m_proxies TA_GUARDED(m_list_lock);
    bool m_zero_handles TA_GUARDED(m_list_lock) = false;
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_PAGER_DISPATCHER_HPP
