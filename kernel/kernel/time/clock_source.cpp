#include <ktl/rwonce.hpp>
#include <logz4/log.hpp>
#include <ours/lockdep.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/clock_source.hpp>
#include <ours/time/tick_device.hpp>

namespace ours::time {
sync::Mutex ClockSource::REGISTERED_SOURCES_MUTEX;
ClockSource::List ClockSource::REGISTERED_SOURCES;
ClockSource *ClockSource::CURRENT_CLKSRC;

ClockSource::ClockSource(char const *name, Vtable const *vtable)
    : m_canary(this)
    , m_vtable(vtable)
    , m_name(name)
    , m_flags()
    , m_ticks_to_ns()
    , m_rating(0)
    , m_freq_hz(0)
    , m_max_cycle(0)
    , m_last_cycle(0) {}

auto ClockSource::enroll(Self *self, usize hz, Rating rating) -> Status {
    self->m_canary.verify<Self>();

    if (hz == 0) {
        log::error("ClockSource {} enrolled with a zero frequency.", self->m_name);
        return Status::InvArgs;
    }

    // ns/tick = 1e9 / hz, reduced so both halves fit the u32 ratio. The
    // reduction is what makes ordinary frequencies fit (1e9/14318180 reduces
    // to 50000000/715909); a frequency above 2^32 Hz would not, and nothing
    // enrolled today comes within a factor of two of that.
    auto numerator = usize(duration_cast<Duration>(1_s).count());
    auto denominator = hz;
    mtl::reduce(numerator, denominator);
    if (numerator > ustl::NumericLimits<u32>::max() ||
        denominator > ustl::NumericLimits<u32>::max()) {
        log::error("ClockSource {}: {} Hz does not fit the tick-to-ns ratio.", self->m_name, hz);
        return Status::InvArgs;
    }
    self->m_rating = rating;
    self->m_freq_hz = hz;
    self->m_ticks_to_ns = mtl::Ratio<u32>(u32(numerator), u32(denominator));

    Self *best = nullptr;
    {
        Guard<sync::Mutex> guard(REGISTERED_SOURCES_MUTEX);

        if (self->m_managed_hook.is_linked()) {
            log::error("ClockSource {} was already enrolled.", self->m_name);
            return Status::BadState;
        }
        self->m_flags |= Flags::Enrolled;
        enqueue_source(self);

        best = select_best_source(false);
    }

    if (!best || CURRENT_CLKSRC == best) {
        return Status::Ok;
    }

    // The steady clock first: it is the one the kernel reads, and a source
    // that the system clock rejects is no reason to leave it empty.
    auto status = SteadyClock::notify(best);
    if (Status::Ok != status) {
        return status;
    }
    status = SystemClock::notify(best);
    if (Status::Ok != status) {
        return status;
    }

    log::info("switched to clock-source {} ({} Hz, rating {})", best->m_name, best->m_freq_hz,
              best->m_rating);
    CURRENT_CLKSRC = best;
    return Status::Ok;
}

auto ClockSource::enqueue_source(Self *self) -> void {
    auto iter = REGISTERED_SOURCES.begin();
    for (auto end = REGISTERED_SOURCES.end(); iter != end; ++iter) {
        if (iter->m_rating < self->m_rating) {
            break;
        }
    }

    (void)REGISTERED_SOURCES.insert(iter, *self);
}

/// The list is kept in priority order by enqueue_source(), so the best
/// source is the first one that is not ruled out. Two things rule one out,
/// the same two as Linux's clocksource_select(): a source marked Unstable
/// (the watchdog's verdict, once there is one), and -- while the tick device
/// runs in oneshot mode -- a source without HpClock, Linux's
/// CLOCK_SOURCE_VALID_FOR_HRES: a coarse source cannot drive high-resolution
/// timers, so oneshot mode must not switch onto one.
auto ClockSource::select_best_source(bool skipcur) -> Self * {
    auto const oneshot = TickDevice::Current::is_mode_oneshot();

    for (auto &source: REGISTERED_SOURCES) {
        if (skipcur && &source == CURRENT_CLKSRC) {
            continue;
        }
        if (!!(source.m_flags & Flags::Unstable)) {
            continue;
        }
        if (oneshot && !(source.m_flags & Flags::HpClock)) {
            continue;
        }
        return &source;
    }

    return nullptr;
}

auto ClockSource::Default::read() -> Ticks {
    return TICKS.load(std::memory_order_relaxed);
}

/// Only ClockSource::Default::CPUNUM calls this, from the periodic tick, so
/// there is exactly one writer and no lock is needed to keep the three
/// fields consistent with each other.
auto ClockSource::Default::advance_ticks(Ticks num_ticks) -> void {
    auto const before = TICKS.load(std::memory_order_relaxed);

    LAST_TICKS = before;
    TICKS.store(before + num_ticks, std::memory_order_relaxed);
    NEXT_PERIOD += num_ticks * NSEC_PER_TICK;
}

auto ClockSource::Default::advance_time(Duration delta) -> void {
    auto const num_ticks = delta / NSEC_PER_TICK;
    advance_ticks(num_ticks);
}

} // namespace ours::time