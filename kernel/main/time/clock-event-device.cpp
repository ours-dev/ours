#include "ours/time/clock-event-device.hpp"

#include <ours/time/tick-device.hpp>
#include <ours/time/mod.hpp>
#include <ours/time/clock.hpp>

#include <ours/mem/cpu-local.hpp>
#include <ours/lockdep.hpp>

namespace ours::time {
    auto to_string(ClockEventDevice::Mode state) -> char const * {
        switch (state) {
            case ClockEventDevice::Mode::Detached: return "Detached";
            case ClockEventDevice::Mode::Shutdown: return "Shutdown";
            case ClockEventDevice::Mode::Periodic: return "Periodic";
            case ClockEventDevice::Mode::Oneshot:  return "Oneshot";
            case ClockEventDevice::Mode::OneshotStopped:   return "OneshotStopped";
        }

        return "BadState";
    }

    ClockEventDevice::ClockEventDevice(char const *name, Vtable const *vtable, 
                                       Feature feature, CpuMask const *cpumask)
        : m_canary()
        , m_vtable(vtable)
        , m_mode(Mode::Detached)
        , m_irqnum(-1)
        , m_name(name)
        , m_feature(feature)
        , m_next_event()
        , m_cpumask(cpumask)
        , m_enrolled_hook()
    {}

    auto ClockEventDevice::entroll(Self &self) -> Status {
        return entroll(self, HZ, 128, HZ);
    }

    auto ClockEventDevice::entroll(Self &self, usize freq, Ticks min_delta, Ticks max_delta) -> Status {
        self.m_canary.verify<Self>();

        if (self.m_cpumask->none()) {
            // FIXME(SmallHuaZi): 
            self.m_cpumask = 0;// CpuMask::of(Cpu::id());
        } else if (self.m_cpumask->all()) {
            self.m_cpumask = &cpu_possible_mask();
        }

        self.configure(freq, min_delta, max_delta);
        {
            Guard<Mutex, IrqSave> guard{s_gced_mutex};
            s_gced.push_back(self);
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

        auto const now = TimePoint<>();
        auto delta = expiry - now;
        if (delta < Duration::zero()) {
            return Status::InvalidArguments;
        }
        delta = ustl::algorithms::clamp(delta, m_min_delta, m_max_delta);
        auto const delta_ticks = time_to_ticks(delta);

        // Delegate to the derived class to set the next event.
        return set_next_event_tick(delta_ticks);
    }

    auto ClockEventDevice::switch_mode(Mode target_mode) -> Status {
        if (m_mode == target_mode) {
            return Status::Ok;
        }

        Status status = Status::Ok;
        switch (target_mode) {
            case Mode::Detached:
            case Mode::Shutdown: {
                status = shutdown();
                m_next_event = TimePoint<>::max();
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