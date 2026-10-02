/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_DISPATCHER_HPP
#define OURS_OBJECT_DISPATCHER_HPP 1

#include <ours/compiler.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/observer.hpp>
#include <ours/object/types.hpp>
#include <ours/rights.hpp>
#include <ours/signals.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/types.hpp>

#include <atomic>

#include <cstdint>

#include <gktl/canary.hpp>
#include <ktl/name.hpp>
#include <ustl/option.hpp>
#include <ustl/rc.hpp>

namespace ours::object {
/// The tag ustl::Rc<Dispatcher>::downcast<T>() checks before it casts.
///
/// ustl already has a checked downcast: a class publishes `TYPE_TAG` and
/// answers it from RefCounted::rc_type_tag(), and downcast<T>() returns an
/// empty Arc on a mismatch instead of a pointer to the wrong type. Handles
/// store an Arc<Dispatcher>, and every system call has to turn one back into
/// the object it expected; the tag is the object's ObjectType, so the answer
/// to "what is this" and the check behind the cast are the same number.
[[nodiscard]]
constexpr auto type_tag(ObjectType const type) -> std::uintptr_t {
    return static_cast<std::uintptr_t>(type);
}

class Dispatcher: public ustl::RefCounted<Dispatcher, ustl::sync::ThreadSafePolicy> {
    using Self = Dispatcher;
    using Base = ustl::RefCounted<Dispatcher, ustl::sync::ThreadSafePolicy>;

  public:
    auto koid() const -> KoId {
        return m_koid;
    }

    /// What kind of object this is, from the tag each concrete dispatcher
    /// publishes (see type_tag()).
    [[nodiscard]]
    auto type() const -> ObjectType {
        return static_cast<ObjectType>(rc_type_tag());
    }

    auto get_lock() -> sync::Mutex & {
        return m_lock;
    }

    auto increase_handle_count() -> void {
        m_handle_count.fetch_add(1, std::memory_order_relaxed);
    }

    auto decrease_handle_count() -> bool {
        return m_handle_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }

    /// Starts |observer| watching for any of |signals|.
    ///
    /// Level-triggered, as Zircon's AddObserver(): if one of them is already
    /// asserted the observer is told at once (on_match) and is never linked.
    /// Without that, a waiter that arrives after the signal went up would
    /// wait for a change that already happened. |handle| identifies the
    /// handle the observer watches through, for cancel().
    auto attach_observer(SignalObserver *, void const *handle, Signals) -> Status;

    /// Stops |observer| watching, if it still is, and reports the signals
    /// asserted now.
    auto detach_observer(SignalObserver *, Signals *) -> Status;

    /// Clears |clear|, then sets |set|, and tells every observer the result
    /// matches. Nothing is told when the set of signals did not change.
    ///
    /// Thread context only: the lock is a Mutex. A producer in interrupt
    /// context (a timer, an interrupt object) is meant to defer to a DPC.
    auto update_state(Signals clear, Signals set) -> void;

    /// Ends every observation that was made through |handle|: the handle is
    /// leaving its process's table -- closed, or consumed by a call that takes
    /// it (channel_write, process_start, ...) -- and nothing there can satisfy
    /// a wait through it any more. Each such observer is told on_cancel().
    /// HandleTable calls it, from unlink_locked() and from clean().
    auto cancel(void const *handle) -> void;

    /// The signals asserted right now. A snapshot: they may change the moment
    /// this returns.
    [[nodiscard]]
    auto signals() const -> Signals {
        return m_signals.load(std::memory_order_acquire);
    }

    virtual auto on_zero_handles() -> void {}

    /// ustl's recycle hook: the last reference to this object is gone. It is
    /// destroyed here, or -- when the calling thread is already destroying
    /// one -- queued for that outer deletion to destroy after it. Public
    /// because ustl::RefCounted::release() has to see it; nothing else calls
    /// it. See the definition.
    static auto rc_recycle(Dispatcher *object) noexcept -> void;

  protected:
    /// RefCounted::destroy() calls ~T() when the last reference goes; the
    /// destructor stays protected otherwise, as MemCache and VmAspace do.
    friend class ustl::RefCounted<Dispatcher, ustl::sync::ThreadSafePolicy>;

    auto notify_observers_locked(Signals signals) -> void;

    /// update_state() for a caller that already holds get_lock(), as a
    /// channel does when it queues a message and raises READABLE in one step.
    auto update_state_locked(Signals clear, Signals set) -> void;

    Dispatcher(u32 magic);
    virtual ~Dispatcher() = default;

    gktl::Canary m_canary;
    KoId m_koid;
    sync::Mutex m_lock;
    std::atomic<u32> m_handle_count;
    std::atomic<Signals> m_signals;

    SignalObserver::List m_observers;

  private:
    /// The next object in the deleting thread's pending list, while this one
    /// waits in it (rc_recycle()). Unused otherwise: an object is only ever
    /// linked here after its last reference is gone.
    Dispatcher *m_next_to_delete = nullptr;
};

template <typename Derived, Rights DefaultRights, Signals ExtraSignals = Signals::None>
class SoloDispatcher: public Dispatcher {
    using Base = Dispatcher;
    using Self = SoloDispatcher;

  public:
    constexpr static auto default_right() -> Rights {
        return DefaultRights;
    }

    SoloDispatcher() : Base(Derived::MAGIC) {}

    ~SoloDispatcher() = default;

    auto verify_canary() const -> void {
        m_canary.verify(Derived::MAGIC);
    }

    [[nodiscard]]
    auto rc_type_tag() const noexcept -> std::uintptr_t final {
        return Derived::TYPE_TAG;
    }
};

/// The lock two peered objects share -- the two ends of one channel.
///
/// Every operation on a peered object touches both ends: a write queues on
/// the peer and raises the peer's READABLE; a close clears the peer's
/// WRITABLE. One lock for the pair is what makes each of those a single step
/// that the other end can never see half done. Zircon's PeerHolder.
///
/// Zircon goes one step further and returns this lock from the dispatcher's
/// own virtual get_lock(), so that the pair's signals are guarded by it too.
/// That is deferred here (design 2.4): the holder lock guards the pair's
/// state, each end keeps its own Dispatcher lock for signals and observers,
/// and the order between them is fixed -- holder first.
class PeerHolder final: public ustl::RefCounted<PeerHolder, ustl::sync::ThreadSafePolicy> {
  public:
    PeerHolder() = default;

    /// const, with the lock mutable, because the ends hold the holder through
    /// an `Arc<PeerHolder> const` -- the link never changes -- and ustl::Rc
    /// makes the pointee of a const Arc const. Taking a lock is not a change
    /// to what the holder is.
    [[nodiscard]]
    auto lock() const -> sync::Mutex & {
        return m_lock;
    }

  private:
    mutable sync::Mutex m_lock;
};

template <typename Derived, Rights DefaultRights, Signals ExtraSignals = Signals::None>
class PeeredDispatcher: public Dispatcher {
    using Base = Dispatcher;
    using Self = PeeredDispatcher;

  public:
    constexpr static auto default_right() -> Rights {
        return DefaultRights;
    }

    auto verify_canary() const -> void {
        m_canary.verify(Derived::MAGIC);
    }

    [[nodiscard]]
    auto rc_type_tag() const noexcept -> std::uintptr_t final {
        return Derived::TYPE_TAG;
    }

    /// The last handle to this end went away: cut the pair apart and tell
    /// the peer.
    ///
    /// Centralised here for every peered type, as in Zircon: Derived supplies
    /// on_zero_handles_locked() (what this end does about itself) and
    /// on_peer_zero_handles_locked() (what the surviving end does), and this
    /// function owns the part they all share -- breaking the reference cycle
    /// in both directions under the holder lock. Each end holds an Arc to the
    /// other, so without this neither would ever be freed.
    ///
    /// The peer's Arc is moved out before the lock and dropped after it, so
    /// that if this was the last reference the peer is destroyed outside the
    /// lock rather than under it.
    auto on_zero_handles() -> void final {
        ustl::Arc<Derived> peer;
        {
            Guard guard{m_holder->lock()};
            peer = ustl::move(m_peer);
            static_cast<Derived *>(this)->on_zero_handles_locked();
            if (peer) {
                static_cast<Self &>(*peer).m_peer = ustl::Arc<Derived>();
                peer->on_peer_zero_handles_locked();
            }
        }
    }

  protected:
    explicit PeeredDispatcher(ustl::Arc<PeerHolder> holder)
        : Base(Derived::MAGIC), m_holder(ustl::move(holder)) {}

    /// Links this end to |peer|. Part of construction: called once, before
    /// either end is reachable from anywhere else, which is why it takes no
    /// lock (Zircon's InitPeer()).
    auto init_peer(ustl::Arc<Derived> peer) -> void {
        DEBUG_ASSERT(!m_peer);
        m_peer_koid = peer->koid();
        m_peer = ustl::move(peer);
    }

    /// The lock this end shares with its peer.
    [[nodiscard]]
    auto peer_lock() -> sync::Mutex & {
        return m_holder->lock();
    }

    /// The other end, or empty once it has lost its last handle. Read under
    /// peer_lock().
    [[nodiscard]]
    auto peer() -> ustl::Arc<Derived> & {
        return m_peer;
    }

    [[nodiscard]]
    auto peer_koid() const -> KoId {
        return m_peer_koid;
    }

  private:
    ustl::Arc<PeerHolder> const m_holder;
    ustl::Arc<Derived> m_peer;
    KoId m_peer_koid = 0;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_DISPATCHER_HPP
