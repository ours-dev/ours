/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_INTERRUPT_DISPATCHER_HPP
#define OURS_OBJECT_INTERRUPT_DISPATCHER_HPP 1

#include <ours/compiler.hpp>
#include <ours/interrupt.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/port_dispatcher.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/types.hpp>

#include <ustl/option.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::object {
/// Rights a freshly created interrupt handle carries: Basic and Io, and
/// Signal, which interrupt_trigger asks for.
constexpr auto RIGHTS_DEFAULT_INTERRUPT = Rights::Basic | Rights::Io | Rights::Signal;

class InterruptDispatcher;
using InterruptDispatcherBase = SoloDispatcher<InterruptDispatcher, RIGHTS_DEFAULT_INTERRUPT>;

/// An interrupt as a kernel object: something a thread waits on for an
/// interrupt to arrive, and acknowledges once it has dealt with it.
///
/// The base of a family, unlike most dispatchers, because what stands behind
/// the interrupt differs: a physical line (InterruptEventDispatcher) or
/// nothing at all (VirtualInterruptDispatcher). What they share is the state
/// machine below, which an interrupt enters through handle_interrupt() or
/// trigger(); what differs is the four operations on the hardware, which each
/// kind supplies.
///
/// The objects:
///   - m_state, where the interrupt is in its life (State). Guarded by
///     m_spinlock.
///   - m_timestamp, the time of the first interrupt not yet handed to a
///     waiter: later ones only fold into it until it is taken. Guarded by
///     m_spinlock.
///   - m_event, an auto-unsignal event the synchronous waiter parks on. Each
///     signal goes to exactly one waiter, or is latched for the next wait
///     when none is parked. A wait that takes an interrupt without parking
///     unsignals it under m_spinlock, so a signal latched for an interrupt
///     that wait has just taken does not stand to wake the next one; a bind
///     unsignals it too, as the port takes over the interrupts it stood
///     for.
///   - m_port and m_port_packet: the port the interrupt is bound to, if
///     any, and the one packet it sends there, embedded here so that sending
///     allocates nothing. m_port is guarded by m_spinlock; the packet's
///     fields are split between m_spinlock and the port's interrupt lock, as
///     PortInterruptPacket says.
///   - m_spinlock, L2 in the design's lock order (L1 -> L2 -> {LI, L3},
///     .agents/tasks/microkernel-interrupts/DESIGN.md). A spin lock, held
///     with interrupts off, because a physical line's handler takes it in
///     interrupt context, under the line's lock, L1. The port's interrupt
///     lock, L3, is taken under it to queue or take back the packet, and a
///     line's controller lock, LI, to mask or unmask the line.
///
/// The interrupt is delivered in one of two ways, never both:
///   - Unbound, a thread takes it with interrupt_wait. Taking it is also the
///     acknowledgement of the one before: the next wait is the only ack
///     there is.
///   - Bound to a port (interrupt_bind), it sends m_port_packet there, and
///     interrupt_ack is the acknowledgement; a wait answers BadState. Until
///     the ack it sends nothing more: later interrupts fold into the pending
///     time, and the ack sends them as the next packet.
class InterruptDispatcher: public InterruptDispatcherBase {
    using Base = InterruptDispatcherBase;
    using Self = InterruptDispatcher;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Interrupt);

    /// Takes the next interrupt, waiting for one if none is pending, and
    /// writes the time it arrived to |out_timestamp|.
    ///
    /// Cancelled once the object is destroyed, before or during the wait;
    /// BadState while another thread is already waiting, and while the
    /// interrupt is bound to a port. The wait can be interrupted (a kill, a
    /// suspend), and then answers with what the wait returned, as the object
    /// was before it.
    [[nodiscard]]
    auto wait(Instant *out_timestamp) -> Status;

    /// Raises the interrupt by hand, as having arrived at |timestamp|: a
    /// waiter is woken, or the next wait returns at once. Bound to a port,
    /// it sends a packet there instead, or, while the last one is owed its
    /// ack, leaves the time for the ack to send. Only a virtual interrupt
    /// can be raised this way (BadState otherwise). Cancelled once
    /// destroyed.
    [[nodiscard]]
    auto trigger(Instant timestamp) -> Status;

    /// Ends the interrupt: the line is masked and let go of, and a thread
    /// waiting on it wakes with Cancelled, as does every later wait and
    /// trigger. A second destroy does nothing.
    ///
    /// Bound to a port, a packet still queued there is taken back, so that
    /// the port never holds a packet of a destroyed interrupt. NotFound when
    /// the interrupt needed an ack and had no packet queued -- port_wait
    /// already took it -- with the interrupt destroyed all the same.
    [[nodiscard]]
    auto destroy() -> Status;

    /// Binds the interrupt to |port|, which delivers its interrupts from now
    /// on as Interrupt packets with |key|. An interrupt pending since the
    /// last wait is sent at once.
    ///
    /// Cancelled once destroyed; BadState while a thread is parked in
    /// wait(); AlreadyBound if it is bound already, to this port or another.
    /// |port| must be one made with BindToInterrupt, which the caller checks.
    [[nodiscard]]
    auto bind(ustl::Arc<PortDispatcher> port, u64 key) -> Status;

    /// Unbinds the interrupt from |port|. A packet still queued there is
    /// taken back, and the interrupt it carried is pending again, for the
    /// next wait or bind to take; a packet port_wait has taken already
    /// stays delivered, and the interrupt still needs its ack.
    ///
    /// NotFound if it is not bound to |port|; Cancelled once destroyed.
    [[nodiscard]]
    auto unbind(ustl::Arc<PortDispatcher> const &port) -> Status;

    /// Acknowledges the packet the interrupt sent: the line is unmasked if
    /// it was masked until now, and an interrupt that arrived since the
    /// packet went out is sent at once as the next packet. Ok, with nothing
    /// to do, when no packet is waiting for an ack.
    ///
    /// BadState when it is not bound to a port, and when there is a next
    /// packet to send but the last one is still queued, unread: the
    /// interrupt then folds into that one. Cancelled once destroyed.
    [[nodiscard]]
    auto ack() -> Status;

    /// The last handle is gone, so nothing can wait or ack any more: the
    /// line is let go of here rather than when the last reference drops.
    /// This is what hands a line back when the driver holding it dies. It
    /// goes through destroy().
    auto on_zero_handles() -> void final;

  protected:
    /// What a kind of interrupt asks of the state machine, fixed when it is
    /// made. The bits needed so far:
    ///   - Virtual: there is no line; trigger() is allowed.
    ///   - UnmaskPrewait: unmask the line when it is acked: when a wait in
    ///     NeedAck is about to park, since that wait is the ack, and in
    ///     ack().
    ///   - MaskPostwait: mask the line when a packet is sent, so that a
    ///     level-triggered line still held high does not fire again before
    ///     the driver has quieted the device and acked.
    enum class Flags : u32 {
        None = 0,
        Virtual = BIT(0),
        UnmaskPrewait = BIT(1),
        MaskPostwait = BIT(2),
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    explicit InterruptDispatcher(Flags flags);
    ~InterruptDispatcher() override = default;

    /// The line behind the interrupt fired: called by a physical kind's
    /// handler, in interrupt context, under the line's lock (L1). It records
    /// the time, now, unless an interrupt is pending already, and delivers
    /// it as trigger() does, masking the line until the ack if MaskPostwait
    /// asks for that. Once the object is destroyed it does nothing: the line
    /// is about to be let go of.
    auto handle_interrupt() -> void;

    /// The four operations on what stands behind the interrupt, each kind's
    /// own. A virtual interrupt has nothing to do in any of them.
    ///
    /// mask_interrupt() and unmask_interrupt() may be called with m_spinlock
    /// held, and mask_interrupt() from handle_interrupt(), in interrupt
    /// context under the line's lock; so neither may take that lock. The two
    /// that tear down, from destroy(), are never called with m_spinlock held.
    virtual auto mask_interrupt() -> void = 0;
    virtual auto unmask_interrupt() -> void = 0;
    virtual auto deactivate_interrupt() -> void = 0;
    virtual auto unregister_interrupt_handler() -> void = 0;

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the canary.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static constexpr auto MAGIC = gktl::magic("InterruptDispatcher");

    /// Where the interrupt is in its life:
    ///   - Idle: nothing pending, no one waiting. Where it starts.
    ///   - Waiting: a thread is parked, or about to park, on m_event. Never
    ///     while bound.
    ///   - Triggered: an interrupt is pending and the next wait takes it.
    ///     Never while bound: a pending interrupt is sent at once.
    ///   - NeedAck: a wait took an interrupt, and the next wait is its ack;
    ///     or, bound, a packet went out, and ack() is its ack. Interrupts
    ///     arriving meanwhile only fold into m_timestamp.
    ///   - Destroyed: for good; every wait and trigger is Cancelled.
    /// The values are not load-bearing: nothing reads a state as a number.
    enum class State : u32 {
        Waiting = 0,
        Destroyed = 1,
        Triggered = 2,
        NeedAck = 3,
        Idle = 4,
    };

    /// wait()'s first half, under m_spinlock: takes a pending interrupt, or
    /// answers why the wait cannot go on, or -- returning none -- moves to
    /// Waiting for the caller to park.
    [[nodiscard]]
    auto begin_wait(Instant *out_timestamp) -> ustl::Option<Status>;

    /// wait()'s second half: parks on m_event. A wait that ends early leaves
    /// the object as it found it, Idle rather than Waiting.
    [[nodiscard]]
    auto park() -> Status;

    /// Delivers the pending interrupt, the time of which the caller has just
    /// recorded: as a packet to the bound port, or by signalling a waiter.
    /// What trigger() and handle_interrupt() share once each has its time.
    TA_REQUIRES(m_spinlock)
    auto deliver_locked() -> void;

    /// Hands the pending interrupt's time to a wait: |out_timestamp| gets it
    /// and nothing is pending any more.
    TA_REQUIRES(m_spinlock)
    auto take_timestamp_locked(Instant *out_timestamp) -> void;

    /// Sends the pending interrupt to the bound port as m_port_packet, masks
    /// the line if MaskPostwait asks for it, and clears the pending time.
    /// False if the packet is still queued from last time: the port reader
    /// has not taken it yet, and this interrupt folds into it. The caller
    /// sets NeedAck.
    TA_REQUIRES(m_spinlock)
    auto send_packet_locked() -> bool;

    sync::Event m_event;
    Flags const m_flags;
    sync::SpinLock m_spinlock;
    State m_state TA_GUARDED(m_spinlock) = State::Idle;
    ustl::Option<Instant> m_timestamp TA_GUARDED(m_spinlock);
    /// Kept after destroy(), so that an ack or an unbind then answers
    /// Cancelled rather than as if never bound; dropped when the interrupt
    /// is unbound or freed.
    ustl::Arc<PortDispatcher> m_port TA_GUARDED(m_spinlock);
    PortInterruptPacket m_port_packet;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_INTERRUPT_DISPATCHER_HPP
