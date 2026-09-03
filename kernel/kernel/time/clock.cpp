#include <ours/time/clock.hpp>
#include <ours/time/clock_source.hpp>

namespace ours::time {
/// A Clock is born without a source, and says so.
///
/// This used to install `ClockSource::Default::get()` here, which dereferences
/// `ClockSource::Default::CLKEVT` -- a `static inline ClockSource *` that only
/// `ClockSource::Default::init()` ever fills, and that runs from `time::init()`.
/// Both Clocks in <ours/time/clock.hpp> are `static inline`, so their
/// constructors run inside `ccrt::init_global_static_objects()`, the very first
/// thing `start_kernel()` does and many init levels ahead of `time::init()`:
/// the source was always null there, and `m_cycle_last = source->read()` was a
/// call through a null vtable. It went unnoticed because low addresses stay
/// mapped -- `x86_init_mmu_early()` aliases them onto the physmap -- so the
/// read returned garbage instead of faulting.
///
/// Nothing is lost by starting empty: `ticks()` already answers zero without a
/// source, and `change_source()` routes the first real one through
/// `override_source()`.
Clock::Clock()
    : m_canary(this)
    ,
    // Left at zero deliberately rather than at an identity 1/1 scaling: the
    // tick-to-nanosecond ratio belongs to the clock source, and pretending
    // ticks are nanoseconds until one arrives would be a wrong answer where
    // update_time()'s assertion is a diagnosable one.
    numerator_(0)
    , denominator_(0)
    , m_cycle_last(0)
    , m_time_shift{}
    , m_time_base{}
    , m_clocksource(nullptr)
    , m_seq(0) {}

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

    // Both preconditions belong to whoever installed the source, and both are
    // silent-wrong rather than loud when they do not hold: no source is a call
    // through a null vtable, and a zero denominator is a #DE several frames
    // away from whatever forgot to set it.
    DEBUG_ASSERT(m_clocksource, "Reading a clock that has no source yet");
    DEBUG_ASSERT(denominator_ != 0, "Clock has no tick-to-nanosecond scaling");

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
    return CLOCK.ticks();
}

auto SteadyClock::now() -> TimePoint {
    auto const base_now = CLOCK.now();
    return TimePoint(base_now.time_since_epoch());
}

auto SystemClock::notify(ClockSource *source) -> Status {
    return CLOCK.change_source(source);
}

} // namespace ours::time