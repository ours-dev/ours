#include <ours/koid.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/dispatcher.hpp>

#include <atomic>

namespace ours::object {
/// m_koid and m_signals were missing from this list, so every dispatcher's
/// koid was whatever the allocator's memory held. That mattered less while
/// nothing read it; it matters as soon as a HandleTable exists, because the
/// table's own koid is the fifth of the five checks a handle value has to
/// pass (docs/architecture/object/handle-lookup.md) -- the one that keeps
/// process A's handle values from resolving in process B. A koid that is
/// garbage is a check that can be passed by accident. KoIdGenerator is the
/// same allocator task::Thread already draws its tid from, so a thread and
/// the dispatcher wrapping it never collide.
Dispatcher::Dispatcher(u32 magic)
    : m_canary(magic)
    , m_koid(KoIdGenerator::generate())
    , m_lock()
    , m_handle_count()
    , m_signals(Signals::None) {}

// Neither of these verifies the canary. They used to call
// m_canary.verify<Self>(), with Self = Dispatcher -- but the canary is seeded
// from the concrete type's MAGIC (SoloDispatcher's constructor passes
// Derived::MAGIC), so on every real object that check compared, say,
// "ChannelDispatcher" against "Dispatcher" and asserted. It stayed hidden only
// because nothing had called either function yet. The base cannot name the
// concrete type; the dispatcher methods that call these do, through
// verify_canary().
auto Dispatcher::attach_observer(SignalObserver *observer, void const *handle, Signals signals)
    -> Status {
    DEBUG_ASSERT(observer);

    Guard guard{get_lock()};

    observer->m_handle = handle;
    observer->m_triggering_signals = signals;

    // Level-triggered: a condition that already holds is reported now, and
    // the observer is never linked. Zircon's Dispatcher::AddObserver() does
    // the same -- a wait for READABLE on a channel with a message queued
    // must not wait for the *next* message.
    auto const current = m_signals.load(std::memory_order_relaxed);
    if (!!(current & signals)) {
        observer->on_match(current);
        return Status::Ok;
    }

    m_observers.push_back(*observer);
    return Status::Ok;
}

auto Dispatcher::detach_observer(SignalObserver *observer, Signals *signals) -> Status {
    DEBUG_ASSERT(observer);

    Guard guard{get_lock()};

    if (signals) {
        *signals = m_signals.load(std::memory_order_acquire);
    }

    if (observer->m_managed_hook.is_linked()) {
        (void)m_observers.erase(m_observers.iterator_to(*observer));
    }

    return Status::Ok;
}

auto Dispatcher::notify_observers_locked(Signals signals) -> void {
    for (auto iter = m_observers.begin(), end = m_observers.end(); iter != end;) {
        auto &observer = *iter;
        if (!(observer.m_triggering_signals & signals)) {
            ++iter;
            continue;
        }
        iter = m_observers.erase(iter);
        observer.on_match(signals);
    }
}
auto Dispatcher::update_state(Signals const clear, Signals const set) -> void {
    Guard guard{get_lock()};
    update_state_locked(clear, set);
}

auto Dispatcher::update_state_locked(Signals const clear, Signals const set) -> void {
    auto const previous = m_signals.load(std::memory_order_relaxed);
    auto const next = (previous & ~clear) | set;
    if (next == previous) {
        return;
    }
    m_signals.store(next, std::memory_order_release);
    notify_observers_locked(next);
}

auto Dispatcher::cancel(void const *const handle) -> void {
    Guard guard{get_lock()};

    auto const current = m_signals.load(std::memory_order_relaxed);
    for (auto iter = m_observers.begin(), end = m_observers.end(); iter != end;) {
        auto &observer = *iter;
        if (observer.m_handle != handle) {
            ++iter;
            continue;
        }
        iter = m_observers.erase(iter);
        observer.on_cancel(current);
    }
}
} // namespace ours::object
