/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYNC_MUTEX_HPP
#define OURS_SYNC_MUTEX_HPP 1

#include <ours/sync/config.hpp>
#include <ours/sync/priv/lock_name_storage.hpp>
#include <ours/task/owned_wait_queue.hpp>
#include <ours/time/types.hpp>

#include <gktl/canary.hpp>
#include <ustl/sync/atomic.hpp>

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

    TA_ACQUIRE() FORCE_INLINE
    auto acquire(Duration spining_duration = Duration{}) -> void;

    TA_RELEASE() auto release() -> void;

    auto is_locked() const -> bool;

    [[nodiscard]]
    TA_ASSERT() auto assert_held() const -> bool;

    auto holder() const -> task::Thread * {
        return get_holder_from(m_state.load(ustl::sync::MemoryOrder::Relaxed));
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

    ustl::sync::Atomic<State> m_state;
    ustl::sync::Atomic<CpuNum> m_cpunum;
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
    acquire_common(spining_duration, TimesliceExtension<false>{});
}

} // namespace ours::sync

#endif // #ifndef OURS_SYNC_MUTEX_HPP