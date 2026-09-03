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
    [[maybe_unused]]
    auto const status = Thread::spawn_etc(KoIdGenerator::generate(), name, dummy_start_routine,
                                          sched::BaseProfile(sched::IDLE_PRIORITY),
                                          dummy_trampoline_routine, m_thread.data());
    DEBUG_ASSERT(Status::Ok == status, "Failed to initialize idler[{}]", m_cpu);

    m_thread->set_state_running();
    m_thread->set_flag_idle();
    m_thread->set_flag_nofree();
}

auto IdleThread::run() -> void {
    for ([[maybe_unused]]
         auto &preemption_state = Thread::Current::get()->preemption_state();
         ;) {
        arch::AutoInterruptDisabler irq_disabler;
        AutoPreemptDisabler preempt_disaber;

        auto const states = m_states.load(MemoryOrder::Acquire);
        if (states.current != states.target) [[likely]] {
            switch (states.target) {
                case State::Offline: {
                    break;
                }

                default: {
                    break;
                }
            }

            // We are hope to transition to running state. So swap out idle thread.
            if (State::Running == states.target) {
                Thread::Current::reschedule();
            }
        } else {
            auto const start_time = time::SteadyClock::now();

            m_arch.idle();

            auto const finish_time = time::SteadyClock::now();

            m_cumulative_idle_time += finish_time - start_time;
        }
    }
}

} // namespace ours::task