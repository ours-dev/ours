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

    /// Where the CPU is. A transition is asked for by setting the target
    /// (transition()). The CPU claims it by moving to Booting or Leaving, and
    /// from then on it can no longer be withdrawn; it ends it by moving to the
    /// target.
    enum class State : u8 {
        /// On the way to Running: at boot until the idle loop first runs, and
        /// from the moment a CPU started again claims its return
        /// (come_back()).
        Booting,
        Running,
        /// On the way to Offline, from the moment the idle loop claims the
        /// transition until the CPU is out (go_offline_and_halt()).
        Leaving,
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
    /// masks, closes its IPI mailbox, stops its tick, and halts
    /// (arch_cpu_halt_offline()). Running: the CPU is started again
    /// (arch_activate_cpu()) and comes back through come_back().
    ///
    /// BadState while another transition is in flight. At |deadline| the
    /// transition is withdrawn unless the CPU has claimed it, and TimeOut
    /// means the CPU is as it was: one asked to go offline goes on running,
    /// and one started again that arrives late halts again by itself. A
    /// transition claimed by then is waited for to its end, without a
    /// deadline: what is left of it is the CPU's own work, done with
    /// interrupts off. The caller serialises transitions of one CPU and does
    /// what has to be done around them (task::take_cpu_offline()).
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

    /// How many times |cpu| has halted in its idle loop since boot, and for
    /// how long in all up to |now|: the halts alone, from just before each to
    /// the moment it ends, not the time other threads ran once it had, and
    /// the part already gone of a halt still in progress at |now|. Read from
    /// any CPU (idle.stats).
    [[nodiscard]]
    static auto num_halts(CpuNum cpu) -> u64;
    [[nodiscard]]
    static auto halted_time(CpuNum cpu, TimePoint<> now) -> Duration;

    /// The CPU has halted since it was last started, after which nothing on
    /// it runs until it is started again: it went offline, or arrived after
    /// its start was withdrawn. What arch_deactivate_cpu() waits for.
    [[nodiscard]]
    auto has_halted() const -> bool {
        return m_halted.load(std::memory_order_acquire);
    }

    /// The CPU is back from being offline: called by the architecture's
    /// start-up path, on this CPU, on the idle thread's stack from its top,
    /// interrupts off, once the hardware is set up again. Takes back up what
    /// going offline put down, in reverse, and goes on idling; or, if
    /// transition() has withdrawn the start, halts again.
    [[noreturn]]
    auto come_back() -> void;

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

    /// Moves the state from |asked| to |on_the_way| (Booting or Leaving),
    /// keeping the target: the CPU's side of the race with transition()'s
    /// withdrawal, which moves it from |asked| back to where it was. One
    /// compare-and-swap decides which of the two happened. False if the
    /// state is no longer |asked|, the transition having been withdrawn.
    auto claim(StateMachine asked, State on_the_way) -> bool;

    /// Takes the CPU offline and halts it, once the idle loop has claimed
    /// the transition. On this CPU, in its idle thread, interrupts off.
    [[noreturn]]
    auto go_offline_and_halt() -> void;

    std::atomic<StateMachine> m_states;
    CpuNum m_cpu;
    ArchIdleThread m_arch;
    ustl::LazyInit<Thread> m_thread;

    /// Signalled by this CPU each time it finishes a transition; unsignalled
    /// by transition() before it asks for the next.
    sync::Event m_transitioned;

    /// See has_halted(). Set by arch_cpu_halt_offline(), cleared by
    /// transition() before it starts the CPU.
    std::atomic<bool> m_halted;

    /// How long this CPU's last halt lasted: the idle governor's guess at the
    /// next one. Only this CPU's idle loop touches it.
    Duration m_last_halt;
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