/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKUP_DETECTOR_HEARTBEAT_NOTE_HPP
#define LOCKUP_DETECTOR_HEARTBEAT_NOTE_HPP 1

#include <ours/types.hpp>

namespace lockup_detector {
/// What one CPU last saw of another CPU's heartbeat, the count of ticks that
/// CPU has taken, at its own once-a-second check.
///
/// Every checking CPU keeps one note per other CPU, private to it: only its
/// own tick writes or reads them, with interrupts off, so a note needs no
/// lock and no atomic. Two CPUs checking the same target each count on
/// their own notes, and neither waits for nor disturbs the other.
///
/// The unit is the check, not the time since the target's last tick: a check
/// comes once per second of the checking CPU's own ticks. A pause of the
/// whole machine, such as a debugger stop or the host going to sleep, stops
/// the checker's ticks too, so it counts no checks, where a timestamp would
/// age every heartbeat at once and fire on resume.
///
/// Pure, so that it runs in host tests: the kernel side
/// (src/checker.cpp) reads the heartbeat and hands it in.
class HeartbeatNote final {
  public:
    /// Takes the target's heartbeat at this check. Returns whether the
    /// target has now gone |threshold| checks in a row without a tick; a
    /// threshold of 0 never finds one.
    ///
    /// A heartbeat that has moved since the last check starts the count
    /// over. A target that has never ticked is not counted at all: its tick
    /// has not started yet, which is not the same as a tick that stopped.
    [[nodiscard]]
    constexpr auto check(u64 const heartbeat, u32 const threshold) -> bool {
        if (heartbeat != m_heartbeat) {
            m_heartbeat = heartbeat;
            m_unchanged = 0;
            return false;
        }
        if (heartbeat == 0) {
            return false;
        }
        ++m_unchanged;
        return threshold != 0 && m_unchanged >= threshold;
    }

    /// The checks in a row so far at which the heartbeat had not moved. In
    /// a report, one per CPU, they say in what order the CPUs went quiet.
    [[nodiscard]]
    constexpr auto unchanged() const -> u32 {
        return m_unchanged;
    }

  private:
    u64 m_heartbeat{0};
    u32 m_unchanged{0};
};

} // namespace lockup_detector

#endif // #ifndef LOCKUP_DETECTOR_HEARTBEAT_NOTE_HPP
