/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKUP_DETECTOR_HEARTBEAT_NOTE_HPP
#define LOCKUP_DETECTOR_HEARTBEAT_NOTE_HPP 1

#include <ours/types.hpp>
#include <ustl/option.hpp>

namespace lockup_detector {
/// What one CPU last saw of another CPU's heartbeat, the count of beats that
/// CPU has had, at its own once-a-second check.
///
/// Every checking CPU keeps one note per other CPU, private to it: only that
/// CPU touches them, always with interrupts off -- its heartbeat timer, which
/// writes and reads them, and the IPI in which it copies them for `lockup status` -- so
/// a note needs no lock and no atomic. Two CPUs checking the same target
/// each count on their own notes, and neither waits for nor disturbs the
/// other.
///
/// The unit is the check, not the time since the target's last beat: a check
/// comes with each beat of the checking CPU's own heartbeat. A pause of the
/// whole machine, such as a debugger stop or the host going to sleep, stops
/// the checker's heartbeat too, so it counts no checks, where a timestamp would
/// age every heartbeat at once and fire on resume.
///
/// It also keeps how close the target has come to being found stuck, for
/// `lockup status` and for setting the threshold on data: the most checks
/// in a row it has gone without a beat, and the fewest beats it has had
/// between two checks. A target that beats as steadily as its checker has
/// about one beat between two checks; one whose CPU the host has starved
/// may have none at a check, and one with interrupts off has none at all.
///
/// Pure, so that it runs in host tests: the kernel side
/// (src/checker.cpp) reads the heartbeat and hands it in.
class HeartbeatNote final {
  public:
    /// Takes the target's heartbeat at this check. Returns whether the
    /// target has now gone |threshold| checks in a row without a beat; a
    /// threshold of 0 never finds one.
    ///
    /// A heartbeat that has moved since the last check starts the count
    /// over. A target that has never beaten is not counted at all: its
    /// heartbeat has not started yet, which is not the same as one that
    /// stopped.
    ///
    /// A heartbeat only grows, so the beats between two checks are the
    /// difference of the two. They are counted from the second check that
    /// sees the target beating: the first one sees every beat since the
    /// target's heartbeat started, which is not one check's worth.
    [[nodiscard]]
    constexpr auto check(u64 const heartbeat, u32 const threshold) -> bool {
        if (m_heartbeat != 0) {
            auto const beats = heartbeat - m_heartbeat;
            if (m_fewest_beats.is_none() || beats < m_fewest_beats.value()) {
                m_fewest_beats = beats;
            }
        }
        if (heartbeat != m_heartbeat) {
            m_heartbeat = heartbeat;
            m_unchanged = 0;
            return false;
        }
        if (heartbeat == 0) {
            return false;
        }
        ++m_unchanged;
        if (m_unchanged > m_most_unchanged) {
            m_most_unchanged = m_unchanged;
        }
        return threshold != 0 && m_unchanged >= threshold;
    }

    /// The checks in a row so far at which the heartbeat had not moved. In
    /// a report, one per CPU, they say in what order the CPUs went quiet.
    [[nodiscard]]
    constexpr auto unchanged() const -> u32 {
        return m_unchanged;
    }

    /// The largest unchanged() has ever been: the closest the target has
    /// come to the threshold.
    [[nodiscard]]
    constexpr auto most_unchanged() const -> u32 {
        return m_most_unchanged;
    }

    /// The fewest beats the target has had between two checks; none until
    /// two checks have seen it beating. 0 once it has gone a check without a
    /// beat, and then most_unchanged() says for how many.
    [[nodiscard]]
    constexpr auto fewest_beats() const -> ustl::Option<u64> {
        return m_fewest_beats;
    }

  private:
    u64 m_heartbeat{0};
    u32 m_unchanged{0};
    u32 m_most_unchanged{0};
    ustl::Option<u64> m_fewest_beats;
};

} // namespace lockup_detector

#endif // #ifndef LOCKUP_DETECTOR_HEARTBEAT_NOTE_HPP
