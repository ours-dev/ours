/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_SUSPEND_TOKEN_DISPATCHER_HPP
#define OURS_OBJECT_SUSPEND_TOKEN_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/types.hpp>

#include <ustl/rc.hpp>

namespace ours::object {
/// Rights a suspend token's handle carries: it can be handed on and looked at,
/// and nothing else. Not duplicated, since one handle is one hold on the
/// thread; not waited on, since nothing about the token ever changes.
constexpr auto RIGHTS_DEFAULT_SUSPEND_TOKEN = Rights::Transfer | Rights::Inspect;

using SuspendTokenDispatcherBase =
    SoloDispatcher<SuspendTokenDispatcher, RIGHTS_DEFAULT_SUSPEND_TOKEN>;

/// A hold on a suspended thread. task_suspend makes one, and the thread stays
/// suspended while it lives; closing its last handle resumes the thread, and
/// is the only way to -- there is no task_resume. Each token is one count of
/// the thread's suspend count (ThreadDispatcher::suspend()), so a thread that
/// several tokens hold runs again only when the last of them goes.
class SuspendTokenDispatcher final: public SuspendTokenDispatcherBase {
    using Base = SuspendTokenDispatcherBase;
    using Self = SuspendTokenDispatcher;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::SuspendToken);

    /// Suspends |thread| and makes the token that holds it, into |out|.
    ///
    /// NotSupported for the calling thread itself, as Zircon refuses it: it
    /// would stop on its way out of the very call that made the token.
    /// Otherwise whatever ThreadDispatcher::suspend() answers -- BadState for
    /// a thread that is dying or dead. OutOfMem when the token cannot be made,
    /// which is checked before anything is suspended.
    static auto create(ustl::Arc<ThreadDispatcher> thread, KernelHandle<Self> *out) -> Status;

    /// The last handle is gone: the thread is resumed.
    auto on_zero_handles() -> void override;

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the
    // canary; MAGIC stays private otherwise, so it needs this base in.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static constexpr auto MAGIC = gktl::magic("SuspendTokenDispatcher");

    SuspendTokenDispatcher();
    ~SuspendTokenDispatcher() override;

    /// The thread held: set once it is suspended, and taken out by the one
    /// on_zero_handles(), so that it is resumed once.
    ustl::Arc<ThreadDispatcher> m_thread TA_GUARDED(get_lock());
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_SUSPEND_TOKEN_DISPATCHER_HPP
