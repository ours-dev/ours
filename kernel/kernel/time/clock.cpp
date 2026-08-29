#include <ours/time/clock.hpp>
#include <ours/time/clock_source.hpp>

namespace ours::time {
    Clock::Clock() {
        // Do not initialize any field by using initializer list.
        // Because the following call will do those things.
        override_source(&ClockSource::Default::get());
    }

    auto Clock::change_source(ClockSource *source) -> Status {
        m_canary.verify<Self>();

        if (!m_clocksource) {
            return override_source(source);
        } else if (source == m_clocksource) {
            return Status::Ok;
        }
        update_time();

        return override_source(source);
    }

    auto Clock::override_source(ClockSource *source) -> Status {
        DEBUG_ASSERT(source);

        m_canary.verify<Self>();
        if (source == m_clocksource) {
            return Status::Ok;
        }

        m_clocksource = source;
        m_cycle_last = source->read();
        return Status::Ok;
    }

    auto Clock::update_time() -> void {
        m_canary.verify<Self>();

        auto const cycles_now = m_clocksource->read();
        auto const cycles_delta = cycles_now - m_cycle_last;

        // Converts ticks to a time duration.
        auto const delta_time = cycles_delta * numerator_ / denominator_;
        m_time_shift += Duration(delta_time);
        m_cycle_last = cycles_now;
    }

    auto Clock::ticks() -> Ticks {
        return m_clocksource ? m_clocksource->read() : Ticks(0);
    }

    auto SteadyClock::ticks() -> Ticks {
        return s_clock.ticks();
    }

    auto SteadyClock::now() -> TimePoint {
        auto const base_now = s_clock.now();
        return TimePoint(base_now.time_since_epoch());
    }

    auto SystemClock::notify(ClockSource *source) -> Status {
        return s_clock.change_source(source);
    }

} // namespace ours::time