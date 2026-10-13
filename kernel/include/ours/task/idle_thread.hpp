/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_IDLE_THREAD_HPP
#define OURS_TASK_IDLE_THREAD_HPP 1

#include <ours/arch/idle_thread.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sync/event.hpp>
#include <ours/task/thread.hpp>

#include <atomic>

#include <ustl/lazy_init.hpp>

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

    /// Asks this CPU's idle thread to take the CPU to |target| and waits
    /// until it has, or until |deadline|. Offline: the scheduler runs the
    /// idle thread at once (has_pending_transition()), which, interrupts
    /// off, moves the CPU's threads away, leaves the active and online
    /// masks, closes its IPI mailbox, stops its tick, and waits there with
    /// interrupts off. Running: that CPU sees the new target, takes it all
    /// back up, and goes on idling.
    ///
    /// BadState while another transition is in flight. TimeOut leaves the
    /// transition asked for: the CPU still makes it. The caller serialises
    /// transitions of one CPU and does what has to be done around them
    /// (task::take_cpu_offline()).
    [[nodiscard]]
    auto transition(State target, time::Deadline deadline) -> Status;

    /// A transition has been asked for and not yet made. Read by the
    /// scheduler, which runs the idle thread while it holds.
    [[nodiscard]]
    auto has_pending_transition() const -> bool {
        auto const states = m_states.load(std::memory_order_acquire);
        return states.current != State::Booting && states.current != states.target;
    }

    /// Where the CPU is: Running, or Offline once a transition there is done.
    [[nodiscard]]
    auto state() const -> State {
        return m_states.load(std::memory_order_acquire).current;
    }

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

    /// Takes the CPU offline, waits there for the target to become Running,
    /// and brings it back. On this CPU, in its idle thread, interrupts off.
    auto go_offline_and_wait() -> void;

    std::atomic<StateMachine> m_states;
    CpuNum m_cpu;
    ArchIdleThread m_arch;
    ustl::LazyInit<Thread> m_thread;

    Duration m_cumulative_idle_time;

    /// Signalled by this CPU each time it finishes a transition; unsignalled
    /// by transition() before it asks for the next.
    sync::Event m_transitioned;
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