#include "ours/time/clock_event_device.hpp"

#include <ours/cpu.hpp>
#include <ours/irq/types.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/mod.hpp>
#include <ours/time/tick_device.hpp>

#include <algorithm>
#include <atomic>

#include <logz4/log.hpp>
namespace ours::time {
namespace {
    /// One local APIC timer per possible CPU, the HPET, the PIT, and room.
    constexpr usize MAX_ENROLLED = 2 * MAX_CPU;

    // NOLINTNEXTLINE(readability-identifier-naming): UPPER_CASE, as every namespace-scope object.
    std::atomic<ClockEventDevice *> ENROLLED[MAX_ENROLLED];
    // NOLINTNEXTLINE(readability-identifier-naming): as above.
    std::atomic<usize> NUM_ENROLLED{0};
} // namespace

auto to_string(ClockEventDevice::Mode state) -> char const * {
    switch (state) {
        case ClockEventDevice::Mode::Detached:
            return "Detached";
        case ClockEventDevice::Mode::Shutdown:
            return "Shutdown";
        case ClockEventDevice::Mode::Periodic:
            return "Periodic";
        case ClockEventDevice::Mode::Oneshot:
            return "Oneshot";
        case ClockEventDevice::Mode::OneshotStopped:
            return "OneshotStopped";
    }

    return "BadState";
}

ClockEventDevice::ClockEventDevice(char const *name, Vtable const *vtable, Feature feature,
                                   Rating rating, CpuMask const *cpumask)
    : m_canary(this)
    , m_vtable(vtable)
    , m_name(name)
    , m_mode(Mode::Detached)
    , m_feature(feature)
    , m_rating(rating)
    , m_irqnum(irq::INVALID_V_IRQ_NUM)
    , m_next_event()
    , m_scaler()
    , m_min_delta_ticks()
    , m_max_delta_ticks()
    , m_min_delta()
    , m_max_delta()
    , m_cpumask(cpumask)
    , m_event_handler()
    , m_failed(false)
    , m_enrolled_hook() {}

auto ClockEventDevice::enroll(Self &self, usize freq, Ticks min_delta, Ticks max_delta) -> void {
    self.m_canary.verify<Self>();

    // A device must be able to do what it advertises.
    //
    // This is also the one guard the generated vtable needs. VTABLE_OF binds
    // slots by *name*, so a driver whose method is misspelled, private, or
    // returning the wrong type does not fail to compile -- it gets a null
    // slot, and the device then reports Unsupported at the first mode switch,
    // several layers away from the mistake. Checking the features against the
    // table at enrollment turns that back into one message naming the device.
    //
    // A driver bug, not something the driver could act on at run time, so it
    // is not answered as a Status: the device is refused, and a debug build
    // stops here.
    char const *missing = nullptr;
    if (!!(self.m_feature & Feature::Periodic) && !self.m_vtable->set_mode_periodic) {
        missing = "claims Periodic but has no set_mode_periodic";
    } else if (!!(self.m_feature & Feature::Oneshot) && !self.m_vtable->set_mode_oneshot) {
        missing = "claims Oneshot but has no set_mode_oneshot";
    } else if (!!(self.m_feature & Feature::TimePoint)) {
        if (!self.m_vtable->set_next_event_time) {
            missing = "claims TimePoint but has no set_next_event_time";
        }
    } else if (!self.m_vtable->set_next_event_tick) {
        missing = "can be armed by neither ticks nor time";
    }
    if (missing) {
        log::error("ClockEventDevice {} {}", self.m_name, missing);
        DEBUG_ASSERT(!missing, "a clock-event driver lacks a method its features claim");
        return;
    }

    if (self.m_cpumask == nullptr || self.m_cpumask->none() ||
        self.m_cpumask->count() == CpuMask::BIT_COUNT) {
        self.m_cpumask = &cpu_possible_mask();
    }

    self.configure(freq, min_delta, max_delta);
    {
        // No IrqSave here, deliberately. GCED_MUTEX is a sync::Mutex, which
        // parks the caller on an OwnedWaitQueue when contended; disabling
        // interrupts around a lock that can block is how a CPU stops coming
        // back. The tag used to be written here and did nothing -- Guard
        // ignored its options -- so removing it changes no behaviour today
        // and keeps it that way now that the options are live.
        Guard<sync::Mutex> guard{GCED_MUTEX};
        GCED.push_back(self);

        // The same devices again, in an array that can be read without the
        // mutex: the device a CPU falls back to is chosen where the mutex
        // cannot be taken (ClockEventDevice::for_each_enrolled()). Only
        // written under the mutex, so there is one writer; never shrinks,
        // since a device is never unenrolled.
        auto const index = NUM_ENROLLED.load(std::memory_order_relaxed);
        if (index < MAX_ENROLLED) {
            ENROLLED[index].store(&self, std::memory_order_relaxed);
            NUM_ENROLLED.store(index + 1, std::memory_order_release);
        } else {
            log::error("ClockEventDevice {}: the table of devices a CPU can fall back to is full",
                       self.m_name);
        }
    }

    // Whether this CPU takes the device is TickDevice's decision, not the
    // driver's: AlreadyBound means it keeps the one it has, which is logged
    // there and is no failure. Anything else stopped the swap part-way.
    //
    // With interrupts off for the whole swap, and not only the last step of
    // it. The boot CPU enrols its local APIC timer with interrupts on, while
    // the HPET is still interrupting it; the swap moves the HPET's handler
    // out, and an interrupt that calls the handler half way through the move
    // reads a torn object. The lock above stays outside this region: it is a
    // mutex that can park the caller.
    Status status = Status::Ok;
    {
        arch::AutoInterruptDisabler const no_interrupts;
        status = TickDevice::Current::get().check_new_device(&self);
    }
    if (Status::Ok != status && Status::AlreadyBound != status) {
        log::error("ClockEventDevice {} could not be put into service on CPU[{}]: {}", self.m_name,
                   Cpu::id(), to_string(status));
    }
}

auto ClockEventDevice::for_each_enrolled(Visitor visit, void *context) -> void {
    // The count is read once, with acquire, so every entry below it was stored
    // before it was counted.
    auto const count = NUM_ENROLLED.load(std::memory_order_acquire);
    for (usize i = 0; i < count; ++i) {
        if (!visit(*ENROLLED[i].load(std::memory_order_relaxed), context)) {
            return;
        }
    }
}

auto ClockEventDevice::configure(usize freq, Ticks min_delta, Ticks max_delta) -> void {
    DEBUG_ASSERT(min_delta < max_delta);

    m_canary.verify<Self>();

    m_min_delta_ticks = min_delta;
    m_max_delta_ticks = max_delta;

    using Ratio = decltype(m_scaler);

    usize n = duration_cast<Duration>(1_s).count();
    usize d = freq;
    mtl::reduce(n, d);
    m_scaler = Ratio{u32(n), u32(d)};

    m_min_delta = ticks_to_time(m_min_delta_ticks);
    m_max_delta = ticks_to_time(m_max_delta_ticks);
}

auto ClockEventDevice::program_event(TimePoint<> expiry, LateDeadline late) -> Status {
    m_canary.verify<Self>();

    if (m_mode == Mode::Shutdown) {
        return Status::BadState;
    }

    // Recorded even when the deadline is then rejected: the device's
    // next_event() is "the deadline last asked for", and the caller that is
    // told TimeOut is about to ask for a later one anyway.
    m_next_event = expiry;

    // A device that takes an absolute deadline is handed it as it is, under
    // either policy, and its own answer is returned. It is the one that knows
    // whether it can still meet the deadline, so a TimePoint driver that could
    // be used to emulate the periodic tick must itself return TimeOut for one
    // already past. No driver enrolls with Feature::TimePoint today.
    if (!!(m_feature & Feature::TimePoint)) {
        return set_next_event_time(expiry);
    }

    // The device counts down from now, so the delta must be measured from
    // now -- the real now, off the steady clock, not a default-constructed
    // TimePoint (which is the epoch, and turned every delta into an absolute
    // time that the clamp below then pinned at m_max_delta).
    auto const now = SteadyClock::now();

    // A deadline that is not ahead of now is refused before anything is
    // written to the hardware, when the caller asked for that.
    if (late == LateDeadline::Reject && expiry <= now) {
        return Status::TimeOut;
    }

    // Under Force, a deadline already in the past is programmed at
    // m_min_delta rather than rejected: the caller is the timer queue, which
    // has no retry loop, and what it wants from a late deadline is an
    // interrupt as soon as the hardware can deliver one. The clamp handles
    // that because a negative delta is below m_min_delta. Under Reject only a
    // deadline that is still ahead gets here, and the clamp can then only
    // move it later, never earlier: a future deadline closer than the device
    // can manage is armed at the closest it can.
    auto const delta = std::clamp(expiry - now, m_min_delta, m_max_delta);

    auto const status = set_next_event_tick(time_to_ticks(delta));
    if (Status::Ok == status) {
        return status;
    }

    // The device refused -- the HPET does when the comparator write may have
    // landed too late to match (see program_min_delta()). Under Reject that
    // is the same verdict as a deadline already past and goes back to the
    // caller unretried; only Force goes on to retry.
    if (late == LateDeadline::Reject) {
        return status;
    }
    return program_min_delta();
}

/// A device that refused a deadline because it was too close is asked again,
/// a little further out each time.
///
/// A driver returns TimeOut when it cannot promise the interrupt it was just
/// asked for -- the HPET does, because its comparator fires on equality and a
/// comparator written late is missed rather than late. There is nobody above
/// to retry: the timer queue programs and moves on. So the retry lives here,
/// and only failing every attempt is an error the caller sees. Only
/// LateDeadline::Force comes here; the emulated periodic tick asks with
/// Reject and treats a refusal as a missed period of its own.
///
/// The delta grows by one m_min_delta per attempt rather than doubling. What
/// pushes an attempt over the edge is an interrupt or an SMI landing in the
/// window, which is a fixed cost, not one that scales with the delta; adding
/// a minimum is enough to clear it and keeps the tick as close to on time as
/// the hardware allows.
auto ClockEventDevice::program_min_delta() -> Status {
    constexpr auto MAX_ATTEMPTS = 10;

    auto delta = Duration::zero();
    for (auto attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        delta += m_min_delta;
        if (delta > m_max_delta) {
            break;
        }

        // Re-read the clock every attempt: the previous one failed because
        // time moved, so measuring from a stale now would fail the same way.
        m_next_event = SteadyClock::now() + delta;
        if (Status::Ok == set_next_event_tick(time_to_ticks(delta))) {
            return Status::Ok;
        }
    }

    // A warning and not an error: what the device's refusal costs is decided
    // above, where it is answered by moving the tick to another device
    // (TickDevice::device_refused()), and that is what says error when there
    // is no other.
    log::warn("ClockEventDevice {}: no deadline could be programmed", m_name);
    return Status::TimeOut;
}

auto ClockEventDevice::switch_mode(Mode target_mode) -> Status {
    if (m_mode == target_mode) {
        return Status::Ok;
    }

    Status status = Status::Ok;
    switch (target_mode) {
        case Mode::Detached:
        case Mode::Shutdown: {
            // A device without a shutdown slot is one with nothing to turn
            // off, so the transition itself still succeeds. Failing it
            // here would leave TickDevice unable to detach such a device.
            status = shutdown();
            if (status == Status::Unsupported) {
                status = Status::Ok;
            }
            m_next_event = INFINITE_TIME;
            break;
        }
        case Mode::Periodic: {
            if (!(m_feature & Feature::Periodic)) {
                return Status::Unsupported;
            }
            status = set_mode_periodic();
            break;
        }
        case Mode::Oneshot: {
            if (!(m_feature & Feature::Oneshot)) {
                return Status::Unsupported;
            }
            status = set_mode_oneshot();
            break;
        }
        case Mode::OneshotStopped: {
            if (!is_mode_oneshot()) {
                log::info("ClockEventDevice on bad work mode={}", to_string(m_mode));
                return Status::BadState;
            }
            status = stop_oneshot();
            break;
        }
    }

    if (status == Status::Ok) {
        m_mode = target_mode;
    }
    return status;
}

auto ClockEventDevice::set_next_event_tick(Ticks ticks) -> Status {
    if (m_vtable->set_next_event_tick) {
        return m_vtable->set_next_event_tick(this, ticks);
    }
    return Status::Unsupported;
}

auto ClockEventDevice::set_next_event_time(TimePoint<> deadline) -> Status {
    if (m_vtable->set_next_event_time) {
        return m_vtable->set_next_event_time(this, deadline);
    }
    return Status::Unsupported;
}

auto ClockEventDevice::set_mode_periodic() -> Status {
    if (m_vtable->set_mode_periodic) {
        return m_vtable->set_mode_periodic(this);
    }
    return Status::Unsupported;
}

auto ClockEventDevice::set_mode_oneshot() -> Status {
    if (m_vtable->set_mode_oneshot) {
        return m_vtable->set_mode_oneshot(this);
    }
    return Status::Unsupported;
}

auto ClockEventDevice::stop_oneshot() -> Status {
    if (m_vtable->stop_oneshot) {
        return m_vtable->stop_oneshot(this);
    }
    return Status::Unsupported;
}

auto ClockEventDevice::resume() -> Status {
    if (m_vtable->resume) {
        return m_vtable->resume(this);
    }
    return Status::Unsupported;
}

auto ClockEventDevice::suspend() -> Status {
    if (m_vtable->suspend) {
        return m_vtable->suspend(this);
    }
    return Status::Unsupported;
}

auto ClockEventDevice::shutdown() -> Status {
    if (m_vtable->shutdown) {
        m_vtable->shutdown(this);
        return Status::Ok;
    }
    return Status::Unsupported;
}

auto ClockEventDevice::broadcast(CpuMask const &targets) -> Status {
    if (m_vtable->broadcast) {
        m_vtable->broadcast(this, targets);
        return Status::Ok;
    }
    return Status::Unsupported;
}

} // namespace ours::time
