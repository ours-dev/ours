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
    INIT_CODE
    auto init_idle_task_percpu(CpuNum cpunum) -> void {
        auto &idler = IdleThread::Current::get(cpunum);
        idler.init(cpunum);

        Thread::Current::set(&idler.thread(), cpunum);

        // Thread::Current now says the idler is what this CPU is running, and
        // the scheduler has to say the same or the two disagree at the first
        // reschedule: reschedule_common() asserts curr == m_runtime.curr_thread,
        // and curr_thread is written by nothing but a context switch. The
        // idler is also the thread this scheduler idles *to*, which is the
        // other field only this call fills in.
        Scheduler::init_first_thread(&idler.thread(), cpunum);
    }
} // namespace

/// Gives the boot CPU a Thread so that Thread::Current works, and nothing more.
///
/// Runs before init_pmm() and init_vmm(), so it must not allocate: create_first()
/// wraps the context already executing here instead of spawning a new one. The
/// idlers, including this CPU's, are built later by init().
INIT_CODE
auto init_early() -> void {
    Thread::create_first(KoIdGenerator::generate(), "boot", BaseProfile(DEFAULT_PRIORITY),
                         BOOT_THREAD.data());
    Thread::Current::set(&*BOOT_THREAD);

    [[maybe_unused]]
    auto thread = Thread::Current::get();
    DEBUG_ASSERT(thread);
}

/// Declares the running CPU ready to be scheduled on.
///
/// Both statements reach through Scheduler::Current and Thread::Current, so
/// they land on whichever CPU executes them and no other -- which is why they
/// cannot be part of init()'s loop over cpu_possible_mask() and why each
/// secondary has to run them for itself on its way in.
///
/// Without set_active() the CPU stays out of the scheduler's Active mask, and
/// select_target_cpu() consults that mask first: an AP that came up perfectly
/// would still never be handed a thread.
///
/// The preemption disable is the matching half of the enable_preemption() at
/// the end of Thread::Current::become_idle(). That one is an unconditional
/// decrement, so an idler reaching it with no disable outstanding wraps the
/// counter to its maximum and is never preemptible again -- the CPU would spin
/// in its idle loop with runnable threads queued on it.
INIT_CODE
auto init_percpu() -> void {
    Scheduler::Current::set_active(true);
    Thread::Current::preemption_state().disable_preemption();
}

INIT_CODE
auto init() -> void {
    // The boot thread is the one thread that could not be given an address
    // space when it was made: init_early() runs before init_vmm(), so at
    // create_first() time there was no kernel address space to name. Every
    // other thread gets one in spawn_etc(). Attach it now, before the first
    // context switch can happen, so that from here on a null m_aspace means
    // "not attached yet" and nothing else.
    //
    // Leaving a kernel thread's aspace null for ever, with the context
    // switch loading the kernel page table when it sees null, is deliberately
    // not done: that makes null carry two meanings that are indistinguishable
    // at the point of use -- "this is a kernel thread" and "nobody has set
    // this yet".
    Thread::Current::get()->set_aspace(mm::VmAspace::kernel_aspace());

    cpu_possible_mask().for_each([](CpuNum cpu) {
        auto &scheduler = Scheduler::Current::get(cpu);
        scheduler.init(cpu);

        init_idle_task_percpu(cpu);
    });

    init_percpu();
}

} // namespace ours::task