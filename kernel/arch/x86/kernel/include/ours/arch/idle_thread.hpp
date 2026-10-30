/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_IDLE_THREAD_HPP
#define OURS_ARCH_IDLE_THREAD_HPP 1

#include <ours/syscall/time.hpp>
#include <ours/types.hpp>

namespace ours {
class ArchIdleThread {
    using Self = ArchIdleThread;

  public:
    /// Waits, interrupts enabled as part of the wait, until an interrupt or,
    /// with MONITOR and MWAIT, a waker's write (wake()); interrupts are on
    /// when it returns. One wait per call: the caller looks again at whether
    /// there is work, whatever ended it.
    ///
    /// With MWAIT, how deep it waits is the idle governor's choice
    /// (idle_states::IdleStates::pick()), from |last_idle|, how long the
    /// last wait lasted, and |until_deadline|, how long until the CPU's
    /// device is due (Duration::max() for never).
    auto idle(Duration last_idle, Duration until_deadline) -> void;

    /// Wakes |cpu| if it is waiting in idle() on its monitor word, by writing
    /// the word, and says whether it did: then it needs no IPI. False on a
    /// CPU without MONITOR and MWAIT, or not waiting. Called by any CPU,
    /// before it sends a reschedule IPI (arch_mp_reschedule_mask()).
    static auto wake(CpuNum cpu) -> bool;

    /// Reads the idle states idle() may wait in, from the boot CPU's CPUID,
    /// once, on the boot CPU, before any other CPU starts: every CPU's idle()
    /// reads them without a lock, and the start of a CPU orders its reads
    /// after this write. Every CPU is taken to have the boot CPU's states.
    static auto find_states() -> void;

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