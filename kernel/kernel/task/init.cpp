#include <ours/task/init.hpp>
#include <ours/task/idle-thread.hpp>
#include <ours/task/scheduler.hpp>

#include <ours/mm/stack.hpp>

#include <ustl/mem/object.hpp>

namespace ours::task {
namespace {
    INIT_CODE
    auto init_idle_task_percpu(CpuNum cpunum) -> void {
        auto &idler = IdleThread::Current::get();
        idler.init();

        Thread::Current::set(&idler.thread());
    }
} // namespace anonymous

    INIT_CODE
    auto init_early() -> void {
        init_idle_task_percpu(0);

        auto thread = Thread::Current::get();
        DEBUG_ASSERT(thread);
    }

    INIT_CODE
    auto init() -> void {
        cpu_possible_mask().for_each([] (CpuNum cpu) {
            auto &scheduler = Scheduler::Current::get(cpu);
            scheduler.init();

            init_idle_task_percpu(cpu);
        });

        Scheduler::Current::set_active(true);
        Thread::Current::preemption_state().disable_preemption();
    }

} // namespace ours::task