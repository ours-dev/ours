#include <ours/cpu.hpp>
#include <ours/ipi_service.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_broadcast.hpp>
#include <ours/time/tick_device.hpp>
#include <ours/time/timer_queue.hpp>

#include <algorithm>

#include <gktl/counter.hpp>
#include <logz4/log.hpp>

// NOLINTNEXTLINE(readability-identifier-naming): named like every other GKTL_COUNTER in the tree.
GKTL_COUNTER(TICK_BROADCAST_PROGRAM_REFUSED, "tick.broadcast.program_refused");
// NOLINTNEXTLINE(readability-identifier-naming): as above.
GKTL_COUNTER(TICK_BROADCAST_IPI_SENT, "tick.broadcast.ipi_sent");

namespace ours::time {
namespace {
    /// The HPET is the only device that can be a group today, and a second is
    /// the PIT, which cannot while the HPET holds the legacy timer line. Two
    /// leaves room for a second HPET channel.
    constexpr usize MAX_GROUPS = 2;

    // NOLINTNEXTLINE(readability-identifier-naming): UPPER_CASE, as every namespace-scope object.
    sync::SpinLock GROUPS_LOCK;
    // NOLINTNEXTLINE(readability-identifier-naming): as above.
    TickBroadcast GROUPS[MAX_GROUPS];
} // namespace

TickBroadcast::TickBroadcast()
    : m_device(nullptr)
    , m_lock()
    , m_handler_installed(false)
    , m_members()
    , m_armed(UNKNOWN_DEADLINE) {
    for (auto &deadline: m_deadlines) {
        deadline = INFINITE_TIME;
    }
}

auto TickBroadcast::for_device(ClockEventDevice &device) -> Self * {
    Guard<sync::SpinLock, IrqSave> guard(GROUPS_LOCK);

    Self *unused = nullptr;
    for (auto &group: GROUPS) {
        if (group.m_device == &device) {
            return &group;
        }
        if (group.m_device == nullptr && unused == nullptr) {
            unused = &group;
        }
    }
    if (unused == nullptr) {
        return nullptr;
    }
    // Before the pointer leaves: anything that has it has a device.
    unused->m_device = &device;
    return unused;
}

auto TickBroadcast::has_member(CpuNum cpu) -> bool {
    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    return m_members.test(cpu);
}

auto TickBroadcast::join(CpuNum cpu) -> Status {
    DEBUG_ASSERT(cpu < MAX_CPU);

    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    if (m_members.none()) {
        // The handler is put on the device once, by the first member the group
        // ever has, and never taken off: it is the group's for as long as the
        // device runs. The device interrupts a CPU that is not the one joining
        // or leaving, and that CPU reads the handler without any lock, so a
        // handler moved out of the device while it runs would be called half
        // moved (see leave()). The device is quiet at this point, a device
        // nobody claimed being detached, so there is no interrupt to read the
        // slot while it is first written.
        DEBUG_ASSERT(m_device->has_event_handler() == m_handler_installed,
                     "{} has an owner other than its broadcast group", m_device->name());
        if (!m_handler_installed) {
            m_device->set_event_handler([this](ClockEventDevice *) { on_device_event(); });
            m_handler_installed = true;
        }

        auto _ = m_device->switch_mode(ClockEventDevice::Mode::Shutdown);
        auto const status = m_device->switch_mode(ClockEventDevice::Mode::Oneshot);
        if (Status::Ok != status) {
            return status;
        }
        m_armed = UNKNOWN_DEADLINE;
    }

    m_members.set(cpu, true);
    m_deadlines[cpu] = INFINITE_TIME;
    return Status::Ok;
}

auto TickBroadcast::leave(CpuNum cpu) -> void {
    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    if (!m_members.test(cpu)) {
        return;
    }

    m_members.set(cpu, false);
    m_deadlines[cpu] = INFINITE_TIME;
    if (m_members.none()) {
        // Stopped, but the handler stays (see join()). An interrupt that was
        // already on its way finds no member whose deadline has come, and
        // returns. Nor can this be called from inside that handler and take
        // it from under its own feet, which happens when a member's queue
        // ends in a refused deadline and the CPU leaves the group there.
        auto _ = m_device->switch_mode(ClockEventDevice::Mode::Detached);
        m_armed = UNKNOWN_DEADLINE;
        return;
    }

    // The leaver may have been the earliest.
    // Counted if the device refused; a leaver has no use for the answer.
    auto _ = rearm_locked();
}

auto TickBroadcast::set_deadline(CpuNum cpu, TimePoint<> deadline) -> Status {
    Guard<sync::SpinLock, IrqSave> guard(m_lock);
    DEBUG_ASSERT(m_members.test(cpu), "CPU[{}] set a deadline on a broadcast group it is not in",
                 cpu);

    m_deadlines[cpu] = deadline;
    return rearm_locked();
}

auto TickBroadcast::rearm_locked() -> Status {
    TimePoint<> earliest = INFINITE_TIME;
    for (CpuNum cpu = 0; cpu < MAX_CPU; ++cpu) {
        if (m_members.test(cpu)) {
            earliest = std::min(earliest, m_deadlines[cpu]);
        }
    }
    if (earliest == m_armed) {
        return Status::Ok;
    }

    auto const status = m_device->arm_or_park(earliest);
    if (Status::Ok != status) {
        // Forgotten, as TimerQueue forgets what its device refused, so that
        // the next call tries again. What a refusal means for the members is
        // docs/repairs/261010/kernel-time.md.
        m_armed = UNKNOWN_DEADLINE;
        TICK_BROADCAST_PROGRAM_REFUSED.add(1);
        return status;
    }
    m_armed = earliest;
    return Status::Ok;
}

/// Runs in the device's interrupt, on whichever CPU its line is routed to,
/// which need not be a member.
///
/// The members whose deadline has come are taken out of the group's books
/// first, under the lock, and the device is re-armed for the rest before any
/// queue runs: a queue can take a long time, and the other members' deadlines
/// should not wait behind it. Each due member puts itself back when its queue
/// ends with set_deadline().
auto TickBroadcast::on_device_event() -> void {
    CpuMask due;
    {
        Guard<sync::SpinLock, IrqSave> guard(m_lock);
        auto const now = SteadyClock::now();

        // It fired, so nothing is armed any more.
        m_armed = INFINITE_TIME;
        for (CpuNum cpu = 0; cpu < MAX_CPU; ++cpu) {
            if (m_members.test(cpu) && m_deadlines[cpu] <= now) {
                due.set(cpu, true);
                m_deadlines[cpu] = INFINITE_TIME;
            }
        }
        auto _ = rearm_locked();
    }

    auto const self = Cpu::id();
    for (CpuNum cpu = 0; cpu < MAX_CPU; ++cpu) {
        if (due.test(cpu) && cpu != self) {
            IpiService::send_event(cpu, IpiEvent::Timer);
            TICK_BROADCAST_IPI_SENT.add(1);
        }
    }
    if (due.test(self)) {
        TimerQueue::get().run_expired_timers();
    }
}

auto TickBroadcast::num_ipis_sent() -> usize {
    return static_cast<usize>(TICK_BROADCAST_IPI_SENT.sum_across_all_cpus());
}

auto TickBroadcast::num_refused_programs() -> usize {
    return static_cast<usize>(TICK_BROADCAST_PROGRAM_REFUSED.sum_across_all_cpus());
}

auto TickBroadcast::handle_ipi() -> void {
    TimerQueue::get().run_expired_timers();
}

} // namespace ours::time
