#include "ours/time/clock_event_device.hpp"

#include <logz4/log.hpp>
#include <ours/irq/types.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/mod.hpp>
#include <ours/time/tick_device.hpp>

#include <algorithm>
namespace ours::time {
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
    , m_enrolled_hook() {}

auto ClockEventDevice::enroll(Self &self, usize freq, Ticks min_delta, Ticks max_delta) -> Status {
    self.m_canary.verify<Self>();

    // A device must be able to do what it advertises.
    //
    // This is also the one guard the generated vtable needs. VTABLE_OF binds
    // slots by *name*, so a driver whose method is misspelled, private, or
    // returning the wrong type does not fail to compile -- it gets a null
    // slot, and the device then reports Unsupported at the first mode switch,
    // several layers away from the mistake. Checking the features against the
    // table at enrollment turns that back into one message naming the device.
    if (!!(self.m_feature & Feature::Periodic) && !self.m_vtable->set_mode_periodic) {
        log::error("ClockEventDevice {} claims Periodic but has no set_mode_periodic", self.m_name);
        return Status::InvArgs;
    }
    if (!!(self.m_feature & Feature::Oneshot) && !self.m_vtable->set_mode_oneshot) {
        log::error("ClockEventDevice {} claims Oneshot but has no set_mode_oneshot", self.m_name);
        return Status::InvArgs;
    }
    if (!!(self.m_feature & Feature::TimePoint)) {
        if (!self.m_vtable->set_next_event_time) {
            log::error("ClockEventDevice {} claims TimePoint but has no set_next_event_time",
                       self.m_name);
            return Status::InvArgs;
        }
    } else if (!self.m_vtable->set_next_event_tick) {
        log::error("ClockEventDevice {} can be armed by neither ticks nor time", self.m_name);
        return Status::InvArgs;
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
    }

    return TickDevice::Current::get().check_new_device(&self);
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

auto ClockEventDevice::program_event(TimePoint<> expiry) -> Status {
    m_canary.verify<Self>();

    if (m_mode == Mode::Shutdown) {
        return Status::BadState;
    }

    m_next_event = expiry;
    if (!!(m_feature & Feature::TimePoint)) {
        return set_next_event_time(expiry);
    }

    // The device counts down from now, so the delta must be measured from
    // now -- the real now, off the steady clock, not a default-constructed
    // TimePoint (which is the epoch, and turned every delta into an absolute
    // time that the clamp below then pinned at m_max_delta).
    //
    // A deadline already in the past is programmed at m_min_delta rather
    // than rejected: the caller is the timer queue, which has no retry loop,
    // and what it wants from a late deadline is an interrupt as soon as the
    // hardware can deliver one. This is the `force` path of Linux's
    // clockevents_program_event(); the clamp handles it because a negative
    // delta is below m_min_delta.
    auto const now = SteadyClock::now();
    auto const delta = std::clamp(expiry - now, m_min_delta, m_max_delta);

    auto status = set_next_event_tick(time_to_ticks(delta));
    if (Status::Ok == status) {
        return status;
    }

    return program_min_delta();
}

/// Linux's clockevents_program_min_delta(): a device that refused a deadline
/// because it was too close is asked again, a little further out each time.
///
/// A driver returns TimeOut when it cannot promise the interrupt it was just
/// asked for -- the HPET does, because its comparator fires on equality and a
/// comparator written late is missed rather than late. There is nobody above
/// to retry: the timer queue programs and moves on, and the periodic tick
/// asserts. So the retry lives here, and only failing every attempt is an
/// error the caller sees.
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

    log::error("ClockEventDevice {}: no deadline could be programmed", m_name);
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
            // off -- Linux treats set_state_shutdown as optional for the same
            // reason -- so the transition itself still succeeds. Failing it
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
