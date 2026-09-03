#include <arch/auto_interrupt_disabler.hpp>
#include <ours/koid.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>
#include <ustl/fmt/format_to.hpp>

using ustl::sync::MemoryOrder;

namespace ours::task {
using mm::CpuLocal;

// Ordered as the members are declared; m_thread and m_arch used to be listed
// the other way round, which -Wreorder rejects under -Werror.
IdleThread::IdleThread(CpuNum cpunum)
    : m_states({.current = State::Booting, .target = State::Running})
    , m_cpu(cpunum)
    , m_arch()
    , m_thread()
    , m_cumulative_idle_time() {}

// |cpunum| is the CPU this idler belongs to, which is not necessarily the one
// running this code: the boot CPU sets up every CPU's idler during task::init().
auto IdleThread::init(CpuNum cpunum) -> void {
    new (this) IdleThread(cpunum);

    constexpr auto dummy_start_routine = [] NO_RETURN() -> i32 {
        auto const cpunum = Cpu::id();
        panic("Idler[{}] enter the dummy start routine", cpunum);
    };

    constexpr auto dummy_trampoline_routine = [] NO_RETURN() -> void {
        auto const cpunum = Cpu::id();
        panic("Idler[{}] enter the dummy trampoline routine", cpunum);
    };

    char name[24];
    ustl::fmt::format_to(name, "Idle/{}\0", m_cpu);

    // TODO(SmallHuaZi) Assign the idle thread an unique Tid.
    //
    // |m_cpu| is handed over as the target CPU because an idler's CPU is not a
    // placement decision to be made for it: there is one per CPU and this is
    // that CPU's. Letting the scheduler choose was both wrong and unanswerable
    // -- wrong because the running CPU is the boot CPU for every iteration of
    // task::init()'s loop, so all of them landed on the BSP, and unanswerable
    // because selection only ever names an online CPU while this loop runs
    // over cpu_possible_mask() before the secondaries are up.
    [[maybe_unused]]
    auto const status = Thread::spawn_etc(KoIdGenerator::generate(), name, dummy_start_routine,
                                          sched::BaseProfile(sched::IDLE_PRIORITY),
                                          dummy_trampoline_routine, m_thread.data(), m_cpu);
    DEBUG_ASSERT(Status::Ok == status, "Failed to initialize idler[{}]", m_cpu);

    m_thread->set_state_running();
    m_thread->set_flag_idle();
    m_thread->set_flag_nofree();
}

auto IdleThread::run() -> void {
    // The idler is Running from the moment it runs. The state machine was
    // constructed {Booting, Running} and nothing ever advanced it, so
    // `current != target` held forever and the old loop rescheduled on every
    // iteration -- with interrupts and preemption both disabled, which is
    // exactly the state in which Scheduler::Current::reschedule() refuses
    // (add_pending() and return), so the CPU spun here silently with
    // interrupts off and no tick could ever arrive. Zircon's IdlePowerThread
    // starts at kActive for the same reason.
    m_states.store({.current = State::Running, .target = State::Running}, MemoryOrder::Release);

    auto &scheduler = Scheduler::Current::get();
    for (;;) {
        // Whether there is work is decided with interrupts off, so a wakeup
        // cannot land between the check and the halt and be slept through:
        // idle() re-enables interrupts only as part of the halt itself
        // (`sti; hlt`), which is atomic against a pending interrupt.
        {
            arch::AutoInterruptDisabler irq_disabler;
            if (!scheduler.num_runnable()) {
                auto const start_time = time::SteadyClock::now();
                m_arch.idle();
                m_cumulative_idle_time += time::SteadyClock::now() - start_time;
                continue;
            }
        }

        // Something is runnable. Reschedule with interrupts and preemption
        // enabled -- Scheduler::Current::reschedule() will not switch from a
        // non-preemptible context, it only records a pending request -- and
        // let the next thread run; we are back here when it is our turn again.
        Thread::Current::reschedule();
    }
}

} // namespace ours::task