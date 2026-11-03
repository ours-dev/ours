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
#include <ours/task/types.hpp>
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
///   - m_waiter, the one thread in a wait that has found nothing to take,
///     from then until its park returns. Guarded by m_spinlock. It is what
///     keeps waiters to one at a time, since m_event wakes one thread per
///     signal: a trigger moves m_state on from Waiting before the waiter has
///     parked, but leaves m_waiter as it is.
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
///     lock, L3, is taken under it to queue or take back the packet. It is
///     never held across unmask_interrupt(), which on a physical line takes
///     L1: under L2 that would be L2 -> L1 against the handler's L1 -> L2.
///
/// What the object owes its line: from each interrupt it delivers until the
/// ack that leaves nothing more to deliver, its driver has a device to
/// service, and a level line must not raise the interrupt again meanwhile.
/// The object does not mask the line for that; a physical line's flow does,
/// told by the handler's answer (WakeThread). The object's part is to say
/// when the debt is cleared, through unmask_interrupt(), once m_spinlock is
/// dropped: an ack() that finds nothing more to send, or a wait in NeedAck
/// that finds nothing pending, as that wait is the ack. An ack that sends
/// the next packet, or a wait that takes the next interrupt, leaves the
/// object owed an ack again, and says nothing.
///
/// A virtual interrupt has no line to tell, and tells whoever waits on it
/// through a signal instead: Idle (<ours/signals.hpp>), up while the object is
/// owed no ack, which sync_signals() keeps equal to the state after every
/// change that moves the debt (trigger(), destroy(), and the moments
/// unmask_interrupt() is called). Setting a signal takes a Mutex, so
/// trigger(), destroy(), ack() and wait() are thread-context calls with no
/// lock held and preemption on.
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
    /// BadState while the interrupt is bound to a port, and while another
    /// thread waits on it: from the moment that thread's wait finds nothing
    /// to take until that thread wakes, whatever arrives meanwhile, so only
    /// one thread is ever parked on the object. A wait from another thread
    /// that comes in after the wake, before the woken thread looks again,
    /// takes the interrupt that woke it, and the woken one waits again. The
    /// wait can be interrupted (a kill, a suspend), and then answers with
    /// what the wait returned, as the object was before it.
    [[nodiscard]]
    auto wait(Instant *out_timestamp) -> Status;

    /// Raises the interrupt by hand, as having arrived at |timestamp|: a
    /// waiter is woken, or the next wait returns at once. Bound to a port,
    /// it sends a packet there instead, or, while the last one is owed its
    /// ack, leaves the time for the ack to send. Only a virtual interrupt
    /// can be raised this way (BadState otherwise). Cancelled once
    /// destroyed. Drops the Idle signal, before it returns.
    [[nodiscard]]
    auto trigger(Instant timestamp) -> Status;

    /// Ends the interrupt: the line is masked and let go of, and a thread
    /// waiting on it wakes with Cancelled, as does every later wait and
    /// trigger. A second destroy does not end it again, but it still waits for
    /// the line, as the first does: the kernel taking the line away
    /// (line_revoked()) destroys the object too, and the removal is using it
    /// until it has let the line go, so every destroy has to wait for that
    /// before its caller may free the object. A virtual interrupt raises
    /// Idle, and keeps it up.
    ///
    /// Bound to a port, a packet still queued there is taken back, so that
    /// the port never holds a packet of a destroyed interrupt. NotFound when
    /// the interrupt needed an ack and had no packet queued -- port_wait
    /// already took it -- with the interrupt destroyed all the same; but not
    /// if the kernel took the line away first, whose destroy answers Ok.
    [[nodiscard]]
    auto destroy() -> Status;

    /// Binds the interrupt to |port|, which delivers its interrupts from now
    /// on as Interrupt packets with |key|. An interrupt pending since the
    /// last wait is sent at once.
    ///
    /// Cancelled once destroyed; BadState while a thread waits in wait(), as
    /// a second wait would answer; AlreadyBound if it is bound already, to
    /// this port or another.
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

    /// Acknowledges the packet the interrupt sent. An interrupt that arrived
    /// since the packet went out is sent at once as the next packet, which is
    /// owed an ack in turn, and a line masked until now stays masked for it;
    /// with none, the object owes nothing more, and the line is unmasked if
    /// it was masked until now. Ok, with nothing to do, when no packet is
    /// waiting for an ack.
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

    /// A function wait() calls on a thread that has found nothing to take,
    /// between begin_wait() and park(), with no lock held.
    using BeforeParkHook = auto (*)(InterruptDispatcher &interrupt) -> void;

    /// Installs |hook|, or none with nullptr, the default. For kernel tests
    /// only: interrupt.test holds a waiter in that gap, where it has claimed
    /// the wait but is not parked yet, to play another thread's trigger and
    /// wait there -- an interleaving no timing makes reliably. The hook is
    /// called for every interrupt object's waits, so it looks at which one
    /// it was called for.
    static auto set_before_park_hook_for_test(BeforeParkHook hook) -> void;

  protected:
    /// What a kind of interrupt asks of the state machine, fixed when the
    /// object is made. The one bit needed so far:
    ///   - Virtual: there is no line; trigger() is allowed.
    /// Whether a line is level-triggered is not among them: the object
    /// behaves the same on either kind, and the line's flow, which follows
    /// the line's mode, decides whether it is masked until the ack.
    enum class Flags : u32 {
        None = 0,
        Virtual = 0_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    explicit InterruptDispatcher(Flags flags);
    ~InterruptDispatcher() override = default;

    /// The line behind the interrupt fired: called by a physical kind's
    /// handler, in interrupt context, under the line's lock (L1). It records
    /// the time, now, unless an interrupt is pending already, and delivers
    /// it as trigger() does. It touches no mask: on a level line the line's
    /// flow has masked the line already, and keeps it masked until the ack
    /// says so (unmask_interrupt()). Once the object is destroyed it does
    /// nothing: the line is about to be let go of.
    auto handle_interrupt() -> void;

    /// The kernel took the line away from this object (irq::Line::revoke()):
    /// the object is destroyed as destroy() would, except that the line is
    /// let go of already, so nothing here detaches. Called with the line's
    /// lock held, in the context of the removal, so it takes only the locks a
    /// handler takes after it, and it wakes the parked waiter with Cancelled,
    /// or takes a queued packet off the bound port; a bound port is told
    /// nothing more. Nothing happens if the object is destroyed already. The
    /// object must not be freed until destroy() has returned, which waits
    /// for the removal to be done with it. For a physical kind only, whose
    /// mask and deactivate work (empty for the kinds that exist) must not
    /// need more than the context of a handler.
    auto line_revoked() -> void;

    /// The four operations on what stands behind the interrupt, each kind's
    /// own. A virtual interrupt has nothing to do in any of them.
    ///   - mask_interrupt(): stop what stands behind the interrupt from
    ///     raising it. destroy() calls it before it lets the line go.
    ///   - unmask_interrupt(): the object owes nothing more for the
    ///     interrupts it has delivered, so what stands behind it may raise
    ///     the next one. Called by ack() as it moves to Idle, and by a wait
    ///     in NeedAck that finds nothing pending, as that wait is the ack;
    ///     never while the object is still owed an ack. A virtual interrupt
    ///     raises Idle here.
    ///   - deactivate_interrupt() and unregister_interrupt_handler(): tear
    ///     down, from destroy().
    ///
    /// None is called with m_spinlock held, nor from handle_interrupt(). A
    /// physical line's unmask_interrupt() takes the line's lock (L1), which
    /// a dispatch holds while the handler takes m_spinlock, and its
    /// unregister_interrupt_handler() waits on that lock for a dispatch in
    /// flight. TA_EXCLUDES has the thread-safety analysis refuse a call made
    /// under m_spinlock, since no lock checker sees L1.
    TA_EXCLUDES(m_spinlock)
    virtual auto mask_interrupt() -> void = 0;
    TA_EXCLUDES(m_spinlock)
    virtual auto unmask_interrupt() -> void = 0;
    TA_EXCLUDES(m_spinlock)
    virtual auto deactivate_interrupt() -> void = 0;
    TA_EXCLUDES(m_spinlock)
    virtual auto unregister_interrupt_handler() -> void = 0;

    /// Makes the Idle signal of a virtual interrupt say what the state says:
    /// up unless the object is owed an ack (Triggered, NeedAck). Nothing for a
    /// physical one. Called after every change that moves the debt: the
    /// creation, a trigger, an ack or a wait that clears it, a destroy.
    ///
    /// It reads the state under m_spinlock, sets the signal, which takes the
    /// dispatcher's Mutex and so needs thread context and no lock held, and
    /// reads the state again; if the debt moved meanwhile it sets the signal
    /// again. So two calls on two CPUs cannot leave the signal behind the
    /// state: the last change to the state is followed by a call that reads
    /// it, and an older call that writes after that one reads the state again
    /// and corrects itself. The signal can lag the state only while a call is
    /// inside the object.
    TA_EXCLUDES(m_spinlock)
    auto sync_signals() -> void;

  private:
    /// Where the interrupt is in its life:
    ///   - Idle: nothing pending, no one waiting. Where it starts.
    ///   - Waiting: a thread is parked, or about to park, on m_event, and
    ///     nothing is pending; m_waiter names it. Never while bound. A
    ///     trigger moves it on to Triggered, which is why the state alone
    ///     cannot say whether someone waits.
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

    /// Whether a step of the state machine cleared what the object owed for
    /// the interrupts it has delivered.
    enum class Debt : u8 {
        Unchanged,
        /// The step was the ack of the last interrupt delivered, and found
        /// nothing more to deliver: unmask_interrupt() is due once
        /// m_spinlock is dropped.
        Cleared,
    };

    /// What begin_wait() leaves wait() to do once m_spinlock is dropped.
    struct WaitStart {
        /// The wait's answer when it goes no further: it took an interrupt,
        /// or cannot go on. None when it is to park.
        ustl::Option<Status> answer;
        /// Cleared only for a wait that is to park.
        Debt debt = Debt::Unchanged;
    };

    /// wait()'s first half, under m_spinlock: takes a pending interrupt, or
    /// answers why the wait cannot go on, or -- with no answer -- moves to
    /// Waiting and makes the calling thread m_waiter, for it to park. A wait
    /// that is to park from NeedAck is the ack of the interrupt the last wait
    /// took, and says that it cleared the debt.
    [[nodiscard]]
    auto begin_wait(Instant *out_timestamp) -> WaitStart;

    /// wait()'s second half: parks on m_event, then gives up m_waiter, and
    /// Waiting with it, however the park ended. A wait that ends early leaves
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

    /// Sends the pending interrupt to the bound port as m_port_packet, and
    /// clears the pending time. False if the packet is still queued from last
    /// time: the port reader has not taken it yet, and this interrupt folds
    /// into it. The caller sets NeedAck.
    TA_REQUIRES(m_spinlock)
    auto send_packet_locked() -> bool;

    sync::Event m_event;
    sync::SpinLock m_spinlock;
    /// Fixed when the object is made, so read without m_spinlock.
    Flags const m_flags;
    State m_state TA_GUARDED(m_spinlock) = State::Idle;
    /// Set by begin_wait() when it gives no answer, cleared by park() when the
    /// park returns; never set while bound. Only compared, never followed:
    /// the thread it names is the one between those two points, and clears
    /// it itself before it leaves wait().
    task::Thread const *m_waiter TA_GUARDED(m_spinlock) = nullptr;
    ustl::Option<Instant> m_timestamp TA_GUARDED(m_spinlock);
    /// Kept after destroy(), so that an ack or an unbind then answers
    /// Cancelled rather than as if never bound; dropped when the interrupt
    /// is unbound or freed.
    ustl::Arc<PortDispatcher> m_port TA_GUARDED(m_spinlock);
    PortInterruptPacket m_port_packet;
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_INTERRUPT_DISPATCHER_HPP
