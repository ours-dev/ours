/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_OBSERVER_HPP
#define OURS_OBJECT_OBSERVER_HPP 1

#include <ours/signals.hpp>
#include <ours/types.hpp>

#include <ustl/collections/intrusive/list.hpp>

namespace ours::object {
class SignalObserver {
    using Self = SignalObserver;

  public:
    // Called when the object's signals come to include one of the triggering
    // signals this observer was attached with.
    //
    // |signals| is every signal the object holds at the match -- its whole
    // state, the value update_state_locked() hands notify_observers_locked()
    // and the one attach_observer() reads -- not only the triggering signals
    // that matched. An observer that wants the intersection takes it itself.
    //
    // At the time this is called, it is safe to delete this object: the
    // caller will not interact with it again.
    //
    // WARNING: This is called under Dispatcher's lock
    virtual auto on_match(Signals signals) -> void = 0;

    // Called when the registered handle (which refers to a handle to the
    // Dispatcher object) is being destroyed/"closed"/transferred. (The
    // object itself may also be destroyed shortly afterwards.)
    //
    // At the time this is call, it is safe to delete this object: the
    // caller will not interact with it again.
    //
    // WARNING: This is called under Dispatcher's lock
    virtual auto on_cancel(Signals signals) -> void = 0;

    // Determine if this observer matches the given port and key.
    //
    // If true, this object will be removed.
    //
    // WARNING: This is called under Dispatcher's lock.
    virtual auto match_key(void const *port, u64 key) -> bool {
        return false;
    }

  protected:
    friend class Dispatcher;

    SignalObserver() = default;

    virtual ~SignalObserver() = default;

    // Dispatcher state, guarded by KernelObject's lock.
    void const *m_handle;
    Signals m_triggering_signals;
    ustl::collections::intrusive::ListMemberHook<> m_managed_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_managed_hook, ManagedOption);

  public:
    USTL_DECLARE_LIST(Self, List, SignalObserver::ManagedOption);
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_OBSERVER_HPP