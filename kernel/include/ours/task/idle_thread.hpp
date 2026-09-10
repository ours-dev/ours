/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_IDLE_THREAD_HPP
#define OURS_TASK_IDLE_THREAD_HPP 1

#include <ours/arch/idle_thread.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/task/thread.hpp>

#include <ustl/lazy_init.hpp>
#include <ustl/sync/atomic.hpp>

namespace ours::task {
class IdleThread {
    using Self = IdleThread;

  public:
    class Current;

    enum class State : u8 {
        Booting,
        Running,
        Offline,
        Suspend,
    };

    // Needed only so CpuLocal::Current's static per-CPU storage can hold
    // one of these before init() placement-news the real state over it.
    IdleThread() = default;
    ~IdleThread() = default;

    auto init(CpuNum cpunum) -> void;

    /// Run loop executed by the CPU's idle/power thread.
    auto run() -> void;

    FORCE_INLINE
    auto thread() -> Thread & {
        return *m_thread;
    }

  private:
    struct alignas(2) StateMachine {
        State current;
        State target;
    };

    explicit IdleThread(CpuNum cpunum);

    ustl::sync::Atomic<StateMachine> m_states;
    CpuNum m_cpu;
    ArchIdleThread m_arch;
    ustl::LazyInit<Thread> m_thread;

    Duration m_cumulative_idle_time;
};

class IdleThread::Current: public mm::CpuLocal::Current<IdleThread> {
  public:
    FORCE_INLINE
    static auto run() -> void {
        return IdleThread::Current::get().run();
    }
};

} // namespace ours::task

#endif // #ifndef OURS_TASK_IDLE_THREAD_HPP