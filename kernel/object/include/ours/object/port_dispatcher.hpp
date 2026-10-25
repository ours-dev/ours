/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PORT_DISPATCHER_HPP
#define OURS_OBJECT_PORT_DISPATCHER_HPP 1

#include <ours/lockdep.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/port.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/sync/semaphore.hpp>
#include <ours/sync/spin_lock.hpp>

#include <gktl/canary.hpp>
#include <ustl/box.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::object {
/// Rights a freshly created port handle carries: Basic without Wait, because
/// a port asserts no signals of its own -- which is also what keeps a port
/// from being waited on through another port -- plus Read to dequeue and
/// Write to queue.
constexpr auto RIGHTS_DEFAULT_PORT = (Rights::Basic & ~Rights::Wait) | Rights::Io;

class PortObserver;

/// An interrupt's packet, which lives inside the interrupt object that sends
/// it and is linked on a port's interrupt queue while it waits to be read.
///
/// Embedded rather than allocated because it is queued from interrupt
/// context, where nothing may be allocated, and because one is enough: an
/// interrupt sends nothing more until its packet is acked
/// (InterruptDispatcher's NeedAck). The hook says whether it is queued; a
/// queued packet is not queued again.
///
/// Which lock covers what:
///   - |key| is written by its interrupt under the interrupt's lock, and only
///     while the packet is not queued; the port reads it under its interrupt
///     lock while it takes the packet off.
///   - |timestamp| and the hook are written and read under the port's
///     interrupt lock, apart from the interrupt reading and clearing the
///     time once it has taken the packet back off the queue, when nothing
///     else can reach it.
struct PortInterruptPacket {
    Instant timestamp = 0;
    u64 key = 0;

    ustl::collections::intrusive::ListMemberHook<> m_queue_hook;
    USTL_DECLARE_HOOK_OPTION(PortInterruptPacket, m_queue_hook, QueueOption);
    USTL_DECLARE_LIST(PortInterruptPacket, List, QueueOption);
};

struct QueuedPacket;

/// A kernel object a port packet lives in, which takes the packet back once
/// the port is done with it (PortDispatcher::queue()).
///
/// The packet is embedded in its owner and not allocated for each send, so
/// that queuing it allocates nothing: what a sender needs that queues while
/// holding locks under which nothing may be allocated, as a pager's proxy
/// does under a VMO's lock. One packet is in flight at a time; the owner keeps
/// whatever is to be sent next until free() hands the packet back.
class PortPacketOwner {
  public:
    /// The port is done with |packet|, which is off every queue: dequeue()
    /// copied it out to a waiter, or the port dropped it with its queue as it
    /// lost its last handle. Called once for every queue() the port accepted,
    /// and with no lock of the port's held, so the owner may queue the packet
    /// again from here.
    virtual auto free(QueuedPacket *packet) -> void = 0;

  protected:
    PortPacketOwner() = default;
    ~PortPacketOwner() = default;
    USTL_NO_MOVEABLE_AND_COPYABLE(PortPacketOwner);
};

/// One packet on a port's queue. Kernel-internal: what crosses the ABI is
/// |packet| alone.
///
/// Three kinds, by where the packet came from, which says what the port
/// counts it in and who frees it once the port is done with it:
///   - User: a port_queue packet, allocated on its own. |owner| is null and
///     the port frees it. Counted in m_user_packets.
///   - Observer: a SignalOne packet, inside the PortObserver that queued it.
///     The observer is |owner|, and goes with it. Counted in
///     m_live_observers.
///   - Kernel: a packet another kernel object embeds and queues with
///     queue(). |owner| takes it back through free(). Counted in nothing: an
///     owner has one packet in flight at most, which bounds them.
struct QueuedPacket {
    enum class Origin : u8 {
        User,
        Observer,
        Kernel,
    };

    PortPacket packet;
    Origin origin = Origin::User;
    PortPacketOwner *owner = nullptr;

    ustl::collections::intrusive::ListMemberHook<> m_queue_hook;
    USTL_DECLARE_HOOK_OPTION(QueuedPacket, m_queue_hook, QueueOption);
    USTL_DECLARE_LIST(QueuedPacket, List, QueueOption);
};

/// A one-shot, level-triggered wait on an object that delivers into a port
/// (object_wait_async).
///
/// It is linked on the watched object's observer list, under that object's
/// lock, until one of three things ends the wait:
///   - the object asserts a signal it waits for: on_match() queues the
///     embedded packet, and the observer goes with it into the port's queue;
///   - the handle it was made through leaves its table: on_cancel() frees it
///     and nothing is queued;
///   - the port has lost its last handle by the time it matches: the packet
///     is dropped and the observer freed.
///
/// on_match() and on_cancel() cannot both run for one observer. Each is
/// called under the watched object's lock by the code that has just unlinked
/// the observer (or, for an immediate match, never linked it), so whichever
/// comes first is the only one that finds it.
///
/// |m_object| is for a race that object lock does not cover: a closing port
/// tearing down its armed observers (PortDispatcher::on_zero_handles()),
/// which runs under the port's lock and then takes the object's. It says
/// whether the observer is still registered, is read and written under the
/// port's lock, and whoever sets it to null frees the observer. A callback
/// that finds it already null leaves the observer alone: the tear-down has
/// it, and frees it once it has taken it off the object.
class PortObserver final: public SignalObserver, public PortPacketOwner {
  public:
    PortObserver(ustl::Arc<PortDispatcher> port, Dispatcher *object, u64 key, Signals trigger);

    // Out of line: the port is incomplete here, and the destructor drops a
    // reference to it.
    ~PortObserver() override;

    auto on_match(Signals signals) -> void override;
    auto on_cancel(Signals signals) -> void override;

    /// The port is done with this observer's packet: the observer goes with
    /// it, which is what a delivered or dropped SignalOne wait is.
    auto free(QueuedPacket *packet) -> void override;

  private:
    friend class PortDispatcher;

    QueuedPacket m_packet;
    ustl::Arc<PortDispatcher> const m_port;
    Dispatcher *m_object;

    /// On the port's m_armed while registered and not yet queued.
    ustl::collections::intrusive::ListMemberHook<> m_armed_hook;
    USTL_DECLARE_HOOK_OPTION(PortObserver, m_armed_hook, ArmedOption);

  public:
    USTL_DECLARE_LIST(PortObserver, ArmedList, ArmedOption);
};

using PortDispatcherBase = SoloDispatcher<PortDispatcher, RIGHTS_DEFAULT_PORT>;

/// A queue of packets that threads wait on: the kernel half of an event
/// loop.
///
/// The objects:
///   - m_packets, the queue, and m_user_packets, how many of those came from
///     port_queue. Guarded by m_port_lock.
///   - m_interrupt_packets, the interrupt queue: the packets of the
///     interrupts bound to this port, each embedded in its interrupt.
///     Guarded by m_interrupt_lock, L3 in the interrupt design's lock order
///     (L1 -> L2 -> {LI, L3}, .agents/tasks/microkernel-interrupts/DESIGN.md),
///     a spin lock held with interrupts off, since an interrupt handler
///     queues under it. Only a port made with BindToInterrupt has one in use.
///   - m_sema, a counting semaphore the waiters sleep on, posted once for
///     every packet queued on either queue. A producer queues under that
///     queue's lock and posts after letting it go; a waiter takes a unit
///     first and a lock after. So a unit is one turn at the queues, not a
///     claim on a packet: a waiter whose packet was taken away
///     (on_zero_handles() empties the queue, and an interrupt takes its
///     packet back when it is destroyed or unbound, without taking units
///     back) finds the queues empty and waits again. The semaphore needs no
///     lock of the port's, which is what lets an interrupt handler post it.
///   - m_flags, the port's yes/no facts. Guarded by m_port_lock. Its one
///     bit, ZeroHandles, is set once the last handle is gone; nothing may
///     queue after that.
///   - m_armed, the observers registered and not yet matched, and
///     m_live_observers, those plus the observers whose packets are queued.
///     Guarded by m_port_lock. on_zero_handles() tears m_armed down, so a
///     closed port does not live on through the waits it delivered to, and
///     PORT_MAX_OBSERVERS bounds the count.
///
/// m_port_lock is its own Mutex, not the Dispatcher's m_lock: once
/// object_wait_async exists, an observer queues a packet while holding the
/// observed object's lock, and "object lock, then port lock" needs two locks
/// that lockdep can tell apart. m_port_lock and m_interrupt_lock are never
/// held together: a dequeue looks at the interrupt queue, lets its lock go,
/// and only then takes the Mutex.
class PortDispatcher final: public PortDispatcherBase {
    using Base = PortDispatcherBase;
    using Self = PortDispatcher;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Port);

    /// Makes a port. |options| is None or BindToInterrupt; anything else is
    /// InvArgs.
    static auto create(PortCreateOptions options, KernelHandle<Self> *out, Rights *rights)
        -> Status;

    /// Whether interrupts may be bound to this port: made with
    /// BindToInterrupt.
    [[nodiscard]]
    auto can_bind_to_interrupt() const -> bool {
        return m_options == PortCreateOptions::BindToInterrupt;
    }

    /// Queues |packet| as a User packet, with every other field as given:
    /// the caller's status and payload are kept.
    ///
    /// ShouldWait when PORT_MAX_USER_PACKETS are already queued; BadHandle
    /// once the port has lost its last handle; NoMemory if the packet cannot
    /// be allocated.
    auto queue_user(PortPacket const &packet) -> Status;

    /// Takes the first packet into |out|: the first on the interrupt queue
    /// if there is one, else the first on the other.
    ///
    /// |deadline| is 0 to poll or INSTANT_INFINITE to wait for ever; any other
    /// value is NotSupported until a program can read a clock, as for
    /// wait_one(). An empty queue answers TimeOut to a poll. A wait sleeps
    /// until a packet arrives, interruptibly: a kill or a suspend ends it with
    /// the status the sleep returned. Each packet queued wakes one waiter; a
    /// waiter that then finds the queues emptied under it -- by
    /// on_zero_handles(), or by an interrupt taking its packet back -- goes
    /// back to sleep.
    auto dequeue(Instant deadline, PortPacket *out) -> Status;

    /// Queues |packet|, a Kernel packet that lives in |packet->owner| and goes
    /// back to it through free() once the port is done with it. It must not be
    /// queued already, here or on another port. Allocates nothing, and is not
    /// bounded by PORT_MAX_USER_PACKETS: each owner has one packet in flight.
    ///
    /// BadHandle once the port has lost its last handle: the packet is not
    /// queued and free() is not called, so the caller still has it. Nobody will
    /// ever read it, and a caller that waits for free() to send what comes
    /// next has to treat the packet as delivered, or wait for ever.
    [[nodiscard]]
    auto queue(QueuedPacket *packet) -> Status;

    /// Takes |packet|, queued with queue(), off the queue if it is still on
    /// it. True: it was, and is the caller's again, with no free() to come.
    /// False: dequeue() already took it, or the port dropped it with its
    /// queue, and free() has been called or is about to be. The unit its
    /// queue() posted stays on the semaphore, as for an interrupt's packet
    /// taken back.
    [[nodiscard]]
    auto cancel_queued(QueuedPacket *packet) -> bool;

    auto on_zero_handles() -> void override;

    ~PortDispatcher() override;

    /// Since boot, across all ports: observers a closing port tore down, and
    /// callbacks (on_match(), on_cancel()) that found their observer already
    /// claimed by such a tear-down and left it alone. The second is the race
    /// m_object exists for; port.test reads both to know its stress really
    /// produced that race rather than only ever one order.
    struct TearDownCounts {
        isize torn_down;
        isize yielded;
    };
    [[nodiscard]]
    static auto tear_down_counts() -> TearDownCounts;

    /// Registers |observer| with this port, as its first step: called by
    /// Dispatcher::attach_port_observer() under the watched object's lock,
    /// before the level check. Registered first so that on_zero_handles() can
    /// always find it -- an observer the port did not know of could be linked
    /// on an object after the port's tear-down had passed, and would never be
    /// freed.
    ///
    /// NoResource when PORT_MAX_OBSERVERS observers are live, armed or
    /// queued; BadHandle if the port has no handles left.
    [[nodiscard]]
    auto register_observer(PortObserver *observer) -> Status;

    /// Queues an interrupt's |packet|, recording |timestamp| in it, and wakes
    /// a waiter. False, and nothing changes, if it is queued already: the
    /// reader has not taken the last one yet, and the interrupt folds into
    /// it.
    ///
    /// May be called in interrupt context, with the interrupt's lock held:
    /// it allocates nothing and takes only m_interrupt_lock and the
    /// semaphore's, both spin locks taken with interrupts off. The packet
    /// goes on the queue whether or not the port still has handles: it is
    /// bounded, one per interrupt, and its interrupt takes it back when it is
    /// destroyed or unbound.
    [[nodiscard]]
    auto queue_interrupt_packet(PortInterruptPacket *packet, Instant timestamp) -> bool;

    /// Takes an interrupt's |packet| off the queue if it is still on it, for
    /// an interrupt that is being destroyed or unbound. Whether it was. Its
    /// unit stays on the semaphore, and the waiter that takes it finds the
    /// queues empty and waits again.
    [[nodiscard]]
    auto remove_interrupt_packet(PortInterruptPacket *packet) -> bool;

  private:
    friend class PortObserver;

    /// Queues |observer|'s packet, reporting |observed|: the observer has
    /// matched, and the object it watched has already unlinked it. From here
    /// on the queue owns the observer.
    ///
    /// Returns the observer for the caller to free instead when the port has
    /// no handles left: no one could ever read the packet, and queuing it
    /// would keep the port alive through the observer's reference with no
    /// handle to break the cycle. The caller frees it once the port lock is
    /// dropped but still under the watched object's lock -- and, on some
    /// paths, a handle table's or a channel pair's lock -- so freeing it may
    /// run ~PortDispatcher there. That is why the destructor takes no lock and
    /// destroys no handle, and why a closing port must tear its observers
    /// down in on_zero_handles(), never in its destructor.
    [[nodiscard]]
    auto queue_observer_packet(PortObserver *observer, Signals observed) -> ustl::Box<PortObserver>;

    /// The observer's wait is cancelled, its handle gone. Returns the observer
    /// for the caller to free if it is still registered (m_object set), which
    /// it then no longer is; empty if the port's tear-down already took it.
    [[nodiscard]]
    auto maybe_reap(PortObserver *observer) -> ustl::Box<PortObserver>;

    explicit PortDispatcher(PortCreateOptions options) : m_options(options) {}

    /// The interrupt queue's first packet, taken off into |out|; false if it
    /// is empty.
    [[nodiscard]]
    auto dequeue_interrupt_packet(PortPacket *out) -> bool;

    /// The port's yes/no facts, as one word: every bit is written under
    /// m_port_lock.
    enum class Flags : u8 {
        None,
        /// on_zero_handles() has run: the queue is gone, and nothing may
        /// queue or register after it.
        ZeroHandles = 0_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    PortCreateOptions const m_options;
    sync::SpinLock m_interrupt_lock;
    PortInterruptPacket::List m_interrupt_packets TA_GUARDED(m_interrupt_lock);
    sync::Mutex m_port_lock;
    QueuedPacket::List m_packets TA_GUARDED(m_port_lock);
    u32 m_user_packets TA_GUARDED(m_port_lock) = 0;
    Flags m_flags TA_GUARDED(m_port_lock) = Flags::None;
    PortObserver::ArmedList m_armed TA_GUARDED(m_port_lock);
    u32 m_live_observers TA_GUARDED(m_port_lock) = 0;
    sync::Semaphore m_sema;
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_PORT_DISPATCHER_HPP
