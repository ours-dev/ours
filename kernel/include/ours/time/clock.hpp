/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_CLOCK_HPP
#define OURS_TIME_CLOCK_HPP 1

#include <ours/chrono.hpp>
#include <ours/status.hpp>
#include <ours/sync/seqlock.hpp>
#include <ours/time/anchor_page.hpp>
#include <ours/time/clock_type.hpp>
#include <ours/time/types.hpp>

#include <gktl/canary.hpp>

namespace ours::time {
/// A clock is an affine map from a ClockSource's counter to nanoseconds,
/// pinned at one point -- the anchor -- so that switching sources does not
/// make time jump.
///
/// now() is a pure function of the counter: it reads the anchor, reads the
/// counter, scales the difference, and writes nothing. The other design
/// accumulates the last counter value and the nanoseconds so far on every tick,
/// which keeps the counter delta small enough for a 32-bit multiplier and a
/// 64-bit product; the price is that reading the clock is coupled to a periodic
/// writer, and the first draft of this class had exactly that coupling in the
/// worst form -- now() itself did the accumulation, so two concurrent readers
/// tore the state. Reading through a pure map instead means the only writer is
/// a source switch, and the delta can grow without bound because
/// ClockSource::ticks_to_time() scales through a 96-bit intermediate.
class Clock {
    using Self = Clock;

  public:
    using TimePoint = TimePoint<Self>;

    Clock();

    /// Zero until a source has been installed: nothing has started counting,
    /// so nothing has elapsed. Early boot reads the clock before time::init()
    /// (page ageing, the scheduler clock) and a zero there is a delta of zero,
    /// which every such caller already copes with.
    [[nodiscard]]
    auto now() const -> TimePoint;

    /// The clock source's raw counter, with none of the scaling now()
    /// applies. Zero before a source has been registered, which is the
    /// only sensible answer when there is nothing to read.
    [[nodiscard]]
    auto ticks() const -> Ticks;

    /// Installs |source|, keeping now() continuous across the switch: the
    /// time under the old source at the moment of the switch becomes the
    /// anchor time under the new one.
    auto change_source(ClockSource *source) -> Status;

    /// Same as change_source() but skips the "already installed" shortcut;
    /// the anchor is re-pinned even when |source| is the current one.
    auto override_source(ClockSource *source) -> Status;

    [[nodiscard]]
    auto source() const -> ClockSource *;

    [[nodiscard]]
    auto reading() const -> AnchorReading;

  private:
    /// The point the affine map is pinned at: reading |ticks| on |source|
    /// meant |time| had elapsed since the epoch.
    ///
    /// Three words, so it is snapshotted as a unit under the seqlock rather
    /// than read field by field; a reader that saw a new |source| with the
    /// old |ticks| would compute nonsense.
    struct Anchor {
        ClockSource *source;
        Ticks ticks;
        Duration time;
    };

    static auto time_of(Anchor const &anchor) -> Duration;

    gktl::Canary m_canary;

    /// A source switch is the only writer. Readers never block on it, which
    /// matters because now() is called from interrupt handlers and from
    /// under the scheduler's locks.
    sync::SeqLock m_seqlock;
    Anchor m_anchor;
};

class SystemClock {
    using Self = SystemClock;

  public:
    using TimePoint = TimePoint<Self>;

    static auto now() -> TimePoint;

    static auto ticks() -> Ticks;

    // For changes on ClockSource is able to notify system clock.
    static auto notify(ClockSource *source) -> Status;

  private:
    static inline Clock CLOCK;
    static inline TimePoint REAL_BASE;
};

class SteadyClock {
    using Self = SteadyClock;

  public:
    using TimePoint = TimePoint<Self>;

    static auto now() -> TimePoint;

    static auto ticks() -> Ticks;

    [[nodiscard]]
    static auto reading() -> AnchorReading;

    /// Called by ClockSource::enroll() when the best available source
    /// changes. This is the clock everything in the kernel reads, so it is
    /// the one that must never be left without a source.
    static auto notify(ClockSource *source) -> Status;

  private:
    static inline Clock CLOCK;
};

} // namespace ours::time

#endif // #ifndef OURS_TIME_CLOCK_HPP
