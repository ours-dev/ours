/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_IDLE_THREAD_HPP
#define OURS_ARCH_IDLE_THREAD_HPP 1

#include <ours/types.hpp>

namespace ours {
class ArchIdleThread {
    using Self = ArchIdleThread;

  public:
    /// Waits, interrupts enabled as part of the wait, until an interrupt or,
    /// with MONITOR and MWAIT, a waker's write (wake()); interrupts are on
    /// when it returns. One wait per call: the caller looks again at whether
    /// there is work, whatever ended it.
    auto idle() -> void;

    /// Wakes |cpu| if it is waiting in idle() on its monitor word, by writing
    /// the word, and says whether it did: then it needs no IPI. False on a
    /// CPU without MONITOR and MWAIT, or not waiting. Called by any CPU,
    /// before it sends a reschedule IPI (arch_mp_reschedule()).
    static auto wake(CpuNum cpu) -> bool;

    /// Whether idle() waits in MWAIT rather than HLT.
    [[nodiscard]]
    static auto waits_in_mwait() -> bool;

    /// How many of |cpu|'s waits a waker's write ended, since boot: each a
    /// reschedule IPI saved. For idle.stats.
    [[nodiscard]]
    static auto num_monitor_wakes(CpuNum cpu) -> u64;
};

} // namespace ours

#endif // #ifndef OURS_ARCH_IDLE_THREAD_HPP