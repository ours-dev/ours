#include <arch/auto-interrupt-disabler.hpp>
#include <ours/koid.hpp>
#include <ours/task/auto-preempt-disabler.hpp>
#include <ours/task/idle-thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/time/clock.hpp>
#include <ustl/fmt/format_to.hpp>

using ustl::sync::MemoryOrder;

namespace ours::task {
using mm::CpuLocal;

IdleThread::IdleThread()
    : m_states({.current = State::Booting, .target = State::Running})
    , m_cpu(Cpu::id())
    , m_thread()
    , m_arch()
    , m_cumulative_idle_time() {}

auto IdleThread::init() -> void {
    new (this) IdleThread();

    constexpr auto const dummy_start_routine = [] NO_RETURN() -> i32 {
        auto const cpunum = Cpu::id();
        panic("Idler[{}] enter the dummy start routine", cpunum);
    };

    constexpr auto const dummy_trampoline_routine = [] NO_RETURN() -> void {
        auto const cpunum = Cpu::id();
        panic("Idler[{}] enter the dummy trampoline routine", cpunum);
    };

    char name[24];
    ustl::fmt::format_to(name, "Idle/{}\0", m_cpu);

    // TODO(SmallHuaZi) Assign the idle thread an unique Tid.
    auto const status = Thread::spawn_etc(KoIdGenerator::generate(), name, dummy_start_routine,
                                          sched::BaseProfile(sched::kIdlePriority),
                                          dummy_trampoline_routine, m_thread.as_ptr_mut());
    DEBUG_ASSERT(Status::Ok == status, "Failed to initialize idler[{}]", m_cpu);

    m_thread->set_state_running();
    m_thread->set_flag_idle();
    m_thread->set_flag_nofree();
}

auto IdleThread::run() -> void {
    for (auto &preemption_state = Thread::Current::get()->preemption_state();;) {
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
                Thread::Current::schedule();
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