#include <ours/koid.hpp>
#include <ours/mm/stack.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/init.hpp>
#include <ours/task/scheduler.hpp>
#include <ustl/lazy_init.hpp>
#include <ustl/mem/object.hpp>

namespace ours::task {
namespace {
    /// The boot CPU's thread until the idlers exist.
    ///
    /// Static storage on purpose: it is constructed before the slab, so there is
    /// nothing to allocate it from. It is not an IdleThread -- the boot context
    /// and the idle loop are separate roles, and IdleThread::init() cannot run
    /// this early anyway because it spawns, and spawning allocates a stack.
    ustl::LazyInit<Thread> BOOT_THREAD;

    /// Brings up the idler of |cpunum|, which need not be the running CPU.
    /// Requires CpuLocal::init() to have filled in the per-CPU offsets.
    INIT_CODE auto init_idle_task_percpu(CpuNum cpunum) -> void {
        auto &idler = IdleThread::Current::get(cpunum);
        idler.init(cpunum);

        Thread::Current::set(&idler.thread(), cpunum);
    }
} // namespace

/// Gives the boot CPU a Thread so that Thread::Current works, and nothing more.
///
/// Runs before init_pmm() and init_vmm(), so it must not allocate: create_first()
/// wraps the context already executing here instead of spawning a new one. The
/// idlers, including this CPU's, are built later by init().
INIT_CODE auto init_early() -> void {
    Thread::create_first(KoIdGenerator::generate(), "boot",
                         sched::BaseProfile(sched::DEFAULT_PRIORITY), BOOT_THREAD.data());
    Thread::Current::set(&*BOOT_THREAD);

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