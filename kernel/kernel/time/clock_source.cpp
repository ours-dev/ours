#include <ktl/rwonce.hpp>
#include <logz4/log.hpp>
#include <ours/lockdep.hpp>
#include <ours/sync/mutex.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/clock_source.hpp>
#include <ours/time/tick_device.hpp>

namespace ours::time {
ClockSource::List ClockSource::g_registered_sources;
sync::Mutex ClockSource::g_registered_sources_mutex;
ClockSource *ClockSource::g_current_clksrc;

ClockSource::ClockSource(char const *name, Vtable const *vtable)
    : m_canary()
    , m_vtable(vtable)
    , m_name(name)
    , m_flags()
    , m_priority(0)
    , m_freq_khz(0)
    , m_max_cycle(0)
    , m_last_cycle(0) {}

auto ClockSource::enroll(Self *self, usize hz) -> Status {
    self->m_canary.verify<Self>();

    if (self->m_managed_hook.is_linked()) {
        log::error("ClockSource {} was already enrolled.", self->m_name);
        return Status::BadState;
    }
    self->m_flags |= Flags::Enrolled;

    enqueue_source(self);

    auto const best = select_best_source(false);
    if (!best || g_current_clksrc == best) {
        return Status::Ok;
    }

    auto status = SystemClock::notify(self);
    if (Status::Ok != status) {
        return status;
    }

    log::info("switched to clock-source {}", best->m_name);
    g_current_clksrc = best;
    return Status::Ok;
}

auto ClockSource::enqueue_source(Self *self) -> void {
    Guard<sync::Mutex> guard(g_registered_sources_mutex);

    auto iter = g_registered_sources.begin();
    for (auto end = g_registered_sources.end(); iter != end; ++iter) {
        if (iter->m_priority < self->m_priority) {
            break;
        }
    }

    (void)g_registered_sources.insert(iter, *self);
}

auto ClockSource::select_best_source(bool skipcur) -> Self * {
    auto const oneshot = TickDevice::Current::is_mode_oneshot();

    auto iter = g_registered_sources.begin(), end = g_registered_sources.end();
    for (; iter != end; ++iter) {
        if (skipcur && &*iter == g_current_clksrc) {
            continue;
        }

        if (!!(iter->m_flags & Flags::HpClock) && oneshot) {
            log::warn("{}", __FUNCTION__);
        }
        break;
    }

    if (iter == end) {
        return 0;
    }

    return &*iter;
}

auto ClockSource::Default::read() -> usize {
    Guard<sync::SpinLock, IrqSave> guard(s_spinlock);
    return ktl::read_once(s_ticks);
}

auto ClockSource::Default::advance_ticks(Ticks num_ticks) -> void {
    Guard<sync::SpinLock, IrqSave> guard(s_spinlock);
    s_last_ticks = s_ticks;
    ktl::write_once(s_ticks, s_ticks + num_ticks);
    s_next_period += num_ticks * NSEC_PER_TICK;
}

auto ClockSource::Default::advance_time(Duration delta) -> void {
    auto const num_ticks = delta / NSEC_PER_TICK;
    advance_ticks(num_ticks);
}

} // namespace ours::time