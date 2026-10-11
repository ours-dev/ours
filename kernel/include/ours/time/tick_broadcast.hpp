/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_TICK_BROADCAST_HPP
#define OURS_TIME_TICK_BROADCAST_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/lockdep.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/syscall/time.hpp>
#include <ours/time/clock_event_device.hpp>

namespace ours::time {
/// One global timer device serving the timer queues of several CPUs, for the
/// CPUs that cannot use a timer device of their own.
///
/// A global device (the HPET) interrupts one CPU, whichever its line is
/// routed to, so it cannot be a tick device for another CPU the way a local
/// APIC timer is. It can still be armed for the earliest deadline of all the
/// CPUs that rely on it, and when it fires, the CPU it interrupted runs its
/// own queue and tells each other CPU whose deadline has come to run its
/// queue, by IPI. That is the whole of the group.
///
///  * A member is a CPU whose TickDevice joined the group. Its timer queue
///    does not program the device; it tells the group its earliest deadline
///    (set_deadline()), and the group arms the device for the earliest of
///    all of them.
///
///  * When the device fires, the handler takes every member whose deadline
///    has come out of the running, re-arms the device for the rest, and only
///    then lets those members run their queues -- itself directly if it is
///    one, the others by IpiEvent::Timer. A member's queue ends by telling
///    the group its next deadline, which puts it back.
///
/// What it is for: a CPU whose own timer device failed (TickDevice::
/// pick_replacement()), and, in time, a CPU whose own device stops in a deep
/// idle state. It is not a way to run a CPU's timers faster: every deadline
/// of a member costs the group's lock, an IPI and the device's latency.
///
/// Locking. One spin lock, taken with interrupts off, covers the member set,
/// the members' deadlines and what the device is armed for. The order is a
/// member's queue lock, then this one: set_deadline() is called by a queue
/// with its lock held. Nothing here takes a queue lock, and the handler
/// drops this lock before it runs a queue or sends an IPI.
class TickBroadcast {
    using Self = TickBroadcast;

  public:
    /// The group of |device|, made at the first call for it. |device| is a
    /// global device (not one CPU's own), able to be armed for one instant.
    /// Null if the groups are all taken, which is two. Takes a lock that
    /// interrupt handlers do not, so it is for the caller to settle before
    /// it turns interrupts off for the swap.
    [[nodiscard]]
    static auto for_device(ClockEventDevice &device) -> Self *;

    /// Makes |cpu| a member. The first member puts the device into service
    /// for the group; the caller then gives it the CPU's deadline through
    /// set_deadline() (TimerQueue::rearm_device()).
    auto join(CpuNum cpu) -> Status;

    /// Takes |cpu| out. The last member to leave puts the device back to
    /// where an unclaimed device is, detached with no handler.
    auto leave(CpuNum cpu) -> void;

    /// |cpu|'s timer queue now needs an interrupt at |deadline|, or at none
    /// for INFINITE_TIME. Arms the device for the earliest of all the
    /// members', and answers what the device answered.
    auto set_deadline(CpuNum cpu, TimePoint<> deadline) -> Status;

    /// The body of the timer IPI: run this CPU's timer queue.
    static auto handle_ipi() -> void;

    /// Timer IPIs the groups have sent since boot, and deadlines their
    /// devices refused. For tests and diagnosis.
    [[nodiscard]]
    static auto num_ipis_sent() -> usize;
    [[nodiscard]]
    static auto num_refused_programs() -> usize;

    [[nodiscard]]
    auto device() const -> ClockEventDevice & {
        return *m_device;
    }

    [[nodiscard]]
    auto has_member(CpuNum cpu) -> bool;

    /// Only for_device() makes groups; the constructor is public for the storage it
    /// keeps them in.
    TickBroadcast();

  private:
    /// What the device is armed for when nothing is known about it: after a
    /// refusal, and before the first arming. The same stand-in as
    /// TimerQueue's.
    static constexpr auto UNKNOWN_DEADLINE = TimePoint<>::min();

    /// The device fired.
    auto on_device_event() -> void;

    /// Arms the device for the earliest member deadline unless it is known to
    /// be armed for exactly that.
    TA_REQUIRES(m_lock)
    auto rearm_locked() -> Status;

    ClockEventDevice *m_device;
    sync::SpinLock m_lock;
    CpuMask m_members TA_GUARDED(m_lock);
    TimePoint<> m_deadlines[MAX_CPU] TA_GUARDED(m_lock);
    TimePoint<> m_armed TA_GUARDED(m_lock);
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_TICK_BROADCAST_HPP
