/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_MUTEX_HPP
#define OURS_SYNC_MUTEX_HPP 1

#include <ours/sync/config.hpp>
#include <ours/sync/priv/lock_name_storage.hpp>
#include <ours/task/owned_wait_queue.hpp>
#include <ours/time/types.hpp>

#include <atomic>

#include <gktl/canary.hpp>

namespace ours::sync {
class TA_CAPABILITY("Mutex") Mutex: public priv::LockNameStorage<Mutex, LOCK_TRACING_ENABLED> {
    using Base = priv::LockNameStorage<Mutex, LOCK_TRACING_ENABLED>;
    using Self = Mutex;

  public:
    enum class State : usize {
        Available = 0,
        Contended = 0_bit,
        Handoff = 1_bit,

        Mask = Contended | Handoff,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(State);

    /// How long a contended acquire spins before it parks: long enough to
    /// cover a short critical section running on another CPU, short against
    /// the cost of a park and a wake.
    ///
    /// The default argument used to be Duration{} -- zero -- which was
    /// harmless only because the spin loop ignored its duration anyway.
    constexpr static auto SPIN_MAX_DURATION = Duration(Microseconds(150));

    TA_ACQUIRE() FORCE_INLINE
    auto acquire(Duration spining_duration = SPIN_MAX_DURATION) -> void;

    TA_RELEASE() auto release() -> void;

    /// Whether any thread holds this mutex. A snapshot: by the time the
    /// caller looks at the answer it may be stale, so it is for diagnostics
    /// and assertions, never for deciding whether to acquire.
    [[nodiscard]]
    auto is_locked() const -> bool;

    /// Asserts that the calling thread holds this mutex, and tells the
    /// thread-safety analysis so.
    ///
    /// The second half is the point where the analysis cannot see the
    /// acquisition: a lock reached through another object -- a child
    /// mapping's aspace lock, which is the parent's -- or taken by a caller
    /// the analysis does not follow. The first half is checked in debug builds
    /// only; the claim to the analysis holds in every build.
    ///
    /// void, not bool. It used to return a bool marked [[nodiscard]], which
    /// is at odds with TA_ASSERT: the annotation says the lock is held once
    /// the call returns, while a bool hands the verdict to the caller to act
    /// on. It was also declared and never defined, so the first caller would
    /// have failed to link.
    TA_ASSERT() auto assert_held() const -> void;

    auto holder() const -> task::Thread * {
        return get_holder_from(m_state.load(std::memory_order_relaxed));
    }

  protected:
    template <bool Enabled>
    struct TimesliceExtension;

    static auto get_holder_from(State state) -> task::Thread * {
        return reinterpret_cast<task::Thread *>(static_cast<usize>(state & ~State::Mask));
    }

    template <bool Enabled>
    auto acquire_common(Duration spining_duration, TimesliceExtension<Enabled>) -> bool;

    template <bool Enabled>
    auto acquire_slowpath(Duration spining_duration, task::Thread *curr,
                          TimesliceExtension<Enabled>) -> bool;

    auto try_release(task::Thread *) -> State;

    auto release_slowpath(task::Thread *, State) -> void;

    gktl::Canary m_canary{this};

  private:
    auto record_initial_assigned_cpu() -> void;

    auto reset_initial_assigned_cpu() -> void;

    std::atomic<State> m_state;
    std::atomic<CpuNum> m_cpunum;
    task::OwnedWaitQueue m_waiters;
};

template <>
struct Mutex::TimesliceExtension<true> {
    Duration value;
};

template <>
struct Mutex::TimesliceExtension<false> {};

/// Using a timeslice extension to avoid being preempted for time-sensitive tasks.
class TA_CAPABILITY("CriticalMutex") CriticalMutex: protected Mutex {
    using Base = Mutex;
    using Self = CriticalMutex;

  public:
  private:
};

FORCE_INLINE
inline auto Mutex::acquire(Duration spining_duration) -> void {
    m_canary.verify<Self>();

    // acquire_common() answers "you still do not hold it" -- true after a park
    // that could not happen, false once the mutex is this thread's. Dropping
    // that answer, which is what this used to do, turned every contended
    // acquire into a silent no-op: the caller walked into the critical section
    // with m_state naming somebody else.
    while (acquire_common(spining_duration, TimesliceExtension<false>{})) {
    }
}

} // namespace ours::sync

#endif // #ifndef OURS_SYNC_MUTEX_HPP