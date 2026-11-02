#include <ours/time/anchor_page.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/clock_source.hpp>

namespace ours::time {
/// A Clock is born without a source, and says so.
///
/// Both Clocks in <ours/time/clock.hpp> are `static inline`, so this runs
/// inside `ccrt::init_global_static_objects()`, the very first thing
/// `start_kernel()` does and many init levels ahead of `time::init()` -- no
/// source can exist yet, and an earlier draft that installed
/// `ClockSource::Default::get()` here was dereferencing a null pointer that
/// the identity map turned into garbage instead of a fault. Starting empty
/// costs nothing: now() and ticks() both answer zero until change_source()
/// pins the first anchor.
Clock::Clock() : m_canary(this), m_seqlock(), m_anchor{nullptr, 0, Duration::zero()} {}

/// The affine map itself. Reading the counter outside the seqlock is fine:
/// the snapshot is internally consistent, sources are never destroyed, and a
/// counter read that lands after a concurrent switch only makes the delta
/// under the *old* source a little larger, which is still the right answer
/// for the snapshot that was taken.
///
/// The unsigned subtraction is what makes a 64-bit counter that has wrapped
/// still read correctly; a narrower counter would need a mask here and a
/// periodic re-anchoring, which is why enroll() only takes 64-bit sources.
auto Clock::time_of(Anchor const &anchor) -> Duration {
    if (!anchor.source) {
        return Duration::zero();
    }

    auto const delta_ticks = anchor.source->read() - anchor.ticks;
    return anchor.time + anchor.source->ticks_to_time(delta_ticks);
}

auto Clock::now() const -> TimePoint {
    m_canary.verify<Self>();

    auto const anchor = m_seqlock.read([this] { return m_anchor; });
    return TimePoint(time_of(anchor));
}

auto Clock::ticks() const -> Ticks {
    m_canary.verify<Self>();

    auto const source = m_seqlock.read([this] { return m_anchor.source; });
    if (!source) {
        return Ticks(0);
    }
    return source->read();
}

auto Clock::source() const -> ClockSource * {
    return m_seqlock.read([this] { return m_anchor.source; });
}

auto Clock::reading() const -> AnchorReading {
    return m_seqlock.read([this] {
        AnchorReading reading;
        reading.nanoseconds = m_anchor.time.count();
        reading.ticks = m_anchor.ticks;
        if (m_anchor.source != nullptr) {
            reading.numerator = m_anchor.source->ns_numerator();
            reading.denominator = m_anchor.source->ns_denominator();
        }
        return reading;
    });
}

auto Clock::change_source(ClockSource *source) -> Status {
    m_canary.verify<Self>();

    if (source == this->source()) {
        return Status::Ok;
    }
    return override_source(source);
}

auto Clock::override_source(ClockSource *source) -> Status {
    DEBUG_ASSERT(source);
    m_canary.verify<Self>();

    // Both reads happen inside the write so no second switch can slip in
    // between "what time is it under the old source" and "pin that time to
    // the new source's counter". Readers that arrive meanwhile spin on the
    // odd sequence for the few instructions this takes.
    m_seqlock.write([&] {
        auto const time_now = time_of(m_anchor);
        m_anchor = Anchor{source, source->read(), time_now};
    });
    return Status::Ok;
}

auto SteadyClock::ticks() -> Ticks {
    return CLOCK.ticks();
}

auto SteadyClock::reading() -> AnchorReading {
    return CLOCK.reading();
}

auto SteadyClock::now() -> TimePoint {
    auto const base_now = CLOCK.now();
    return TimePoint(base_now.time_since_epoch());
}

auto SteadyClock::notify(ClockSource *source) -> Status {
    auto const status = CLOCK.change_source(source);
    if (status == Status::Ok) {
        publish_anchor(CLOCK.reading());
    }
    return status;
}

/// The wall clock: the same counter as the steady clock, shifted by whatever
/// REAL_BASE says the epoch was.
///
/// REAL_BASE is zero and nothing sets it, because nothing reads an RTC yet,
/// so today this answers exactly what SteadyClock does. That is the right
/// degenerate answer -- "the system booted at the epoch" -- rather than a
/// declaration with no definition, which is what these two were.
auto SystemClock::now() -> TimePoint {
    return TimePoint(REAL_BASE.time_since_epoch() + CLOCK.now().time_since_epoch());
}

auto SystemClock::ticks() -> Ticks {
    return CLOCK.ticks();
}

auto SystemClock::notify(ClockSource *source) -> Status {
    return CLOCK.change_source(source);
}

} // namespace ours::time
