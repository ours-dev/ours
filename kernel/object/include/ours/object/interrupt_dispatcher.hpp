/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_INTERRUPT_DISPATCHER_HPP
#define OURS_OBJECT_INTERRUPT_DISPATCHER_HPP 1

#include <ours/compiler.hpp>
#include <ours/interrupt.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/sync/event.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/types.hpp>

#include <ustl/option.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::object {
/// Zircon ZX_DEFAULT_INTERRUPT_RIGHTS: Basic and Io, and Signal, which
/// interrupt_trigger asks for.
constexpr auto RIGHTS_DEFAULT_INTERRUPT = Rights::Basic | Rights::Io | Rights::Signal;

class InterruptDispatcher;
using InterruptDispatcherBase = SoloDispatcher<InterruptDispatcher, RIGHTS_DEFAULT_INTERRUPT>;

/// An interrupt as a kernel object: something a thread waits on for an
/// interrupt to arrive, and acknowledges once it has dealt with it. Zircon's
/// InterruptDispatcher (kernel/object/interrupt_dispatcher.cc).
///
/// The base of a family, unlike most dispatchers, because what stands behind
/// the interrupt differs: a physical line (InterruptEventDispatcher, once
/// physical lines arrive) or nothing at all (VirtualInterruptDispatcher).
/// What they share is the state machine below; what differs is the four
/// operations on the hardware, which each kind supplies.
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
///     that wait has just taken does not stand to wake the next one.
///   - m_spinlock, L2 in the design's lock order (L1 -> L2 -> {LI, L3},
///     .agents/tasks/microkernel-interrupts/DESIGN.md). A spin lock, held
///     with interrupts off, because a physical line's handler takes it in
///     interrupt context.
///
/// The interrupt is triggered -- by interrupt_trigger for a virtual one --
/// and a thread takes it with interrupt_wait. Taking it is also the
/// acknowledgement of the one before: with no port bound, the next wait is
/// the only ack there is (Zircon interrupt.fidl, zx_interrupt_wait).
class InterruptDispatcher: public InterruptDispatcherBase {
    using Base = InterruptDispatcherBase;
    using Self = InterruptDispatcher;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Interrupt);

    /// Takes the next interrupt, waiting for one if none is pending, and
    /// writes the time it arrived to |out_timestamp|. Zircon's
    /// WaitForInterrupt().
    ///
    /// Cancelled once the object is destroyed, before or during the wait;
    /// BadState while another thread is already waiting. The wait can be
    /// interrupted (a kill, a suspend), and then answers with what the wait
    /// returned, as the object was before it.
    [[nodiscard]]
    auto wait(Instant *out_timestamp) -> Status;

    /// Raises the interrupt by hand, as having arrived at |timestamp|: a
    /// waiter is woken, or the next wait returns at once. Only a virtual
    /// interrupt can be raised this way (BadState otherwise). Cancelled once
    /// destroyed. Zircon's Trigger().
    [[nodiscard]]
    auto trigger(Instant timestamp) -> Status;

    /// Ends the interrupt: the line is masked and let go of, and a thread
    /// waiting on it wakes with Cancelled, as does every later wait and
    /// trigger. A second destroy does nothing. Zircon's Destroy().
    [[nodiscard]]
    auto destroy() -> Status;

    /// The last handle is gone, so nothing can wait or ack any more: the
    /// line is let go of here rather than when the last reference drops.
    /// This is what hands a line back when the driver holding it dies.
    /// Zircon's on_zero_handles(), which calls Destroy() too.
    auto on_zero_handles() -> void final;

  protected:
    /// What a kind of interrupt asks of the state machine, fixed when it is
    /// made. Zircon's InterruptDispatcher::Flags, with the bits it needs so
    /// far and Zircon's values:
    ///   - Virtual: there is no line; trigger() is allowed.
    ///   - UnmaskPrewait: unmask the line when a wait in NeedAck is about to
    ///     park, since that wait is the ack.
    enum class Flags : u32 {
        None = 0,
        Virtual = BIT(0),
        UnmaskPrewait = BIT(1),
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    explicit InterruptDispatcher(Flags flags);
    ~InterruptDispatcher() override = default;

    /// The four operations on what stands behind the interrupt, each kind's
    /// own (Zircon interrupt_dispatcher.h, the pure virtuals). A virtual
    /// interrupt has nothing to do in any of them.
    ///
    /// mask_interrupt() and unmask_interrupt() may be called with m_spinlock
    /// held; the two that tear down, from destroy(), never are.
    virtual auto mask_interrupt() -> void = 0;
    virtual auto unmask_interrupt() -> void = 0;
    virtual auto deactivate_interrupt() -> void = 0;
    virtual auto unregister_interrupt_handler() -> void = 0;

  private:
    // SoloDispatcher's constructor reads Derived::MAGIC to build the canary.
    template <typename, Rights, Signals>
    friend class SoloDispatcher;

    static constexpr auto MAGIC = gktl::magic("InterruptDispatcher");

    /// Where the interrupt is in its life. Zircon's InterruptState, whose
    /// numbering this keeps:
    ///   - Idle: nothing pending, no one waiting. Where it starts.
    ///   - Waiting: a thread is parked, or about to park, on m_event.
    ///   - Triggered: an interrupt is pending and the next wait takes it.
    ///   - NeedAck: a wait took an interrupt; the next wait is its ack.
    ///     Interrupts arriving meanwhile only fold into m_timestamp.
    ///   - Destroyed: for good; every wait and trigger is Cancelled.
    enum class State : u32 {
        Waiting = 0,
        Destroyed = 1,
        Triggered = 2,
        NeedAck = 3,
        Idle = 4,
    };

    /// wait()'s first half, under m_spinlock: takes a pending interrupt, or
    /// answers why the wait cannot go on, or -- returning none -- moves to
    /// Waiting for the caller to park. Zircon's BeginWaitForInterrupt().
    [[nodiscard]]
    auto begin_wait(Instant *out_timestamp) -> ustl::Option<Status>;

    /// wait()'s second half: parks on m_event. A wait that ends early leaves
    /// the object as it found it, Idle rather than Waiting. Zircon's
    /// DoWaitForInterruptBlock().
    [[nodiscard]]
    auto park() -> Status;

    /// Hands the pending interrupt's time to a wait: |out_timestamp| gets it
    /// and nothing is pending any more.
    TA_REQUIRES(m_spinlock)
    auto take_timestamp_locked(Instant *out_timestamp) -> void;

    sync::Event m_event;
    Flags const m_flags;
    sync::SpinLock m_spinlock;
    State m_state TA_GUARDED(m_spinlock) = State::Idle;
    ustl::Option<Instant> m_timestamp TA_GUARDED(m_spinlock);
};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_INTERRUPT_DISPATCHER_HPP
