#include <ours/mm/stack.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/init.hpp>
#include <ours/task/scheduler.hpp>
#include <ustl/mem/object.hpp>

namespace ours::task {
namespace {
    /// Brings up the idler of the CPU this code is running on.
    ///
    /// This is the form the *_early path has to use. Those interfaces exist to
    /// light a facility up on the boot CPU alone, before the machinery that
    /// serves every CPU is there -- at that point CpuLocal has only installed
    /// the current-CPU offset, so a CPU-indexed lookup has nothing to resolve
    /// against yet.
    INIT_CODE auto init_idle_task_current_cpu() -> void {
        auto &idler = IdleThread::Current::get();
        idler.init(Cpu::id());

        Thread::Current::set(&idler.thread());
    }

    /// Brings up the idler of |cpunum|, which need not be the running CPU.
    /// Requires CpuLocal::init() to have filled in the per-CPU offsets.
    INIT_CODE auto init_idle_task_percpu(CpuNum cpunum) -> void {
        auto &idler = IdleThread::Current::get(cpunum);
        idler.init(cpunum);

        Thread::Current::set(&idler.thread(), cpunum);
    }
} // namespace

INIT_CODE auto init_early() -> void {
    init_idle_task_current_cpu();

    [[maybe_unused]]
    auto thread = Thread::Current::get();
    DEBUG_ASSERT(thread);
}

INIT_CODE auto init() -> void {
    cpu_possible_mask().for_each([](CpuNum cpu) {
        auto &scheduler = Scheduler::Current::get(cpu);
        scheduler.init();

        init_idle_task_percpu(cpu);
    });

    Scheduler::Current::set_active(true);
    Thread::Current::preemption_state().disable_preemption();
}

} // namespace ours::task