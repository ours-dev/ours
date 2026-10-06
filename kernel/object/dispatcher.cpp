#include <ours/koid.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/task/thread.hpp>

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

/// Every observer is gone by now. An observer is linked only while the handle
/// it was attached through lives, and that handle holds a reference to this
/// object; a handle cancels its observers when it leaves its table
/// (HandleTable) and, should one ever be destroyed some other way, as it is
/// destroyed (~Handle). An observer still linked here would be one whose
/// owner -- a waiting thread's stack, or a port -- frees it later, into a list
/// that no longer exists.
Dispatcher::~Dispatcher() {
    DEBUG_ASSERT(m_observers.empty(), "object (koid {}) destroyed with observers attached", m_koid);
}

/// The last reference is gone: destroy the object, without letting a chain of
/// objects that own one another recurse down the stack.
///
/// A dispatcher can hold the last reference to another dispatcher -- a
/// channel's queued messages hold handles, and a handle holds its object; a
/// process holds its handle table -- so destroying one can release another,
/// whose destructor releases a third. Done directly, each link is one more
/// frame of destructors on the stack that dropped the first reference, and
/// the length of the chain is the program's to choose: a channel end sent
/// through a channel sent through a channel, a thousand deep, then closed.
/// This turns that recursion into a loop. The first release on a thread
/// runs the deletions and records a pending list on the thread; any release
/// that happens during them finds the list and queues its object instead of
/// destroying it, and the loop takes the queued objects one by one after the
/// object in hand is gone. The stack depth is that of one destructor,
/// whatever the chain.
///
/// Before there is a current thread (early boot) there is nowhere to keep
/// the list, and the object is destroyed at once; no dispatcher chains exist
/// that early.
///
/// The list's head is a local of the outermost call, and the thread only
/// points at it (Thread::pending_deletions()) while that call runs, by a
/// scope that withdraws it on the way out. It is pushed and popped at its
/// front, so queued objects go last-in first-out.
auto Dispatcher::rc_recycle(Dispatcher *object) noexcept -> void {
    auto *const thread = task::Thread::Current::get();
    if (thread == nullptr) {
        object->rc_finish();
        return;
    }

    auto *const pending = thread->pending_deletions();
    if (pending != nullptr) {
        // A deletion further up this thread's stack owns the loop.
        pending->list.push_front(*object);
        return;
    }

    // Declared after |deletions|, so the scope withdraws it from the thread
    // before the list itself is destroyed.
    PendingDeletions deletions;
    task::Thread::PendingDeletionsScope const scope{*thread, deletions};
    // Each destruction may release other dispatchers, which land on the
    // list. Each is popped before it is destroyed: its hook requires, as it
    // is destroyed, that it is no longer linked.
    object->rc_finish();
    while (!deletions.list.empty()) {
        auto next = deletions.list.pop_front();
        next->rc_finish();
    }
}

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
    // the observer is never linked. A wait for READABLE on a channel with a
    // message queued must not wait for the *next* message.
    auto const current = m_signals.load(std::memory_order_relaxed);
    if (!!(current & signals)) {
        observer->on_match(current);
        return Status::Ok;
    }

    m_observers.push_back(*observer);
    return Status::Ok;
}

auto Dispatcher::attach_port_observer(PortObserver *const observer, PortDispatcher &port,
                                      void const *const handle, Signals const signals) -> Status {
    DEBUG_ASSERT(observer);

    Guard guard{get_lock()};

    if (auto const status = port.register_observer(observer); status != Status::Ok) {
        return status;
    }

    observer->m_handle = handle;
    observer->m_triggering_signals = signals;

    // Level-triggered, as attach_observer(): a condition that already holds
    // queues the packet now, and the observer is never linked.
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
        auto _ = m_observers.erase(m_observers.iterator_to(*observer));
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
