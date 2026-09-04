#include <gktl/init_hook.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/dpc.hpp>
#include <ours/lockdep.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>
#include <ustl/fmt/format_to.hpp>
#include <ustl/option.hpp>

#include <new>

namespace ours {
namespace {
    constexpr char const *LEVEL_NAMES[static_cast<usize>(DpcPriority::Count)] = {
        "Low",
        "Normal",
        "High",
    };
} // namespace

auto DpcRunner::Queue::start(char const *name, CpuNum cpu) -> Status {
    // A captureless lambda so this converts to a plain function pointer: the
    // queue travels as the thread's argument rather than being bound into the
    // entry point, which is what used to allocate.
    m_thread = task::Thread::spawn(
        name, [](void *arg) -> i32 { return static_cast<Queue *>(arg)->do_work(); }, this,
        task::DEFAULT_PRIORITY);
    if (!m_thread) {
        return Status::NoResource;
    }

    m_thread->set_cpu_affinity_hard(CpuMask::of(cpu));
    m_thread->detach();
    m_thread->resume();
    return Status::Ok;
}

/// Interrupts stay off for the whole critical section, which is what every
/// other queue in the tree does (TimerQueue takes its own lock exactly this
/// way) and what this one needs now that the interrupt exit really preempts.
///
/// With a plain Guard the tick could land inside the section and switch the
/// holder away; a spinlock is not released by a context switch, so the next
/// thread on this CPU to reach the queue -- Thread::Current::exit() queueing
/// its teardown Dpc -- found the lock held by a thread that was no longer
/// running and died on SpinLock::acquire()'s !is_held(). Nothing else can
/// release it, since the holder is only rescheduled by this same CPU.
auto DpcRunner::Queue::enqueue(Dpc &dpc, CpuNum cpu, Level level) -> void {
    Guard<sync::SpinLock, IrqSave> guard{m_lock};

    DEBUG_ASSERT(Dpc::State::Idle == DpcRunner::state_of(dpc), "Dpc is already queued or running");

    DpcRunner::mark_queued(dpc, cpu, level);
    m_list.push_back(dpc);
    m_event.signal();
}

auto DpcRunner::Queue::cancel(Dpc &dpc) -> Status {
    Guard<sync::SpinLock, IrqSave> guard{m_lock};

    auto const state = DpcRunner::state_of(dpc);
    if (Dpc::State::Running == state) {
        return Status::BadState;
    }
    if (Dpc::State::Idle == state) {
        return Status::NotFound;
    }

    m_list.erase(dpc);
    DpcRunner::mark_idle(dpc);
    return Status::Ok;
}

auto DpcRunner::Queue::do_work() -> i32 {
    while (true) {
        [[maybe_unused]]
        auto const status = m_event.wait();
        DEBUG_ASSERT(Status::Ok == status);

        Dpc *next = nullptr;
        {
            Guard<sync::SpinLock, IrqSave> guard{m_lock};

            auto front = m_list.front();
            if (front.is_none()) {
                m_event.unsignal();
                continue;
            }

            next = &front.value();
            m_list.pop_front();

            // The queue is done with this Dpc *here*, while the lock is still
            // held -- not after the callback returns.
            //
            // A Dpc may be destroyed by its own callback: the one on this
            // path lives on the exiting thread's kernel stack and the callback
            // frees that stack (Thread::Current::exit()). Marking it idle
            // afterwards, as this used to, wrote through a dangling pointer
            // and hung the boot at the first thread exit. Nothing below
            // touches |next| except the invoke() itself, which the object is
            // still alive for.
            //
            // Zircon reaches the same place from the other side: its worker
            // copies the Dpc onto its own stack under the lock and invokes the
            // copy (DpcRunner::Queue::DoWork), so the original is likewise
            // never touched once it leaves the list. Ours needs no copy
            // because it reads nothing after the call.
            //
            // The visible consequence is that State::Running is no longer
            // observable and cancel() of an already-popped Dpc reports
            // NotFound rather than BadState -- which is Zircon's behaviour
            // too: once a Dpc has left the list it cannot be cancelled.
            DpcRunner::mark_idle(*next);
        }

        next->invoke();
    }
    return 0;
}

auto DpcRunner::init() -> void {
    auto &runner = Current::get();

    // Construct this CPU's runner before touching it. CpuLocal hands out
    // zero-filled storage, and a zeroed Queue is not an empty Queue: its
    // intrusive list header is {null, null} rather than self-pointing, and
    // its Event's canary is 0. Nothing noticed until the first enqueue --
    // do_work() parks on the event without ever inspecting the list, and
    // List::insert() does not consult empty(), it links against the header
    // directly, so link_before() wrote through the header's null prev and
    // took a page fault at the first Dpc ever queued.
    //
    // Every other per-CPU object in this tree is built the same way:
    // Scheduler::init() opens with `new (this) Self()`, IdleThread::init()
    // with `new (this) IdleThread(cpunum)`. This one simply never did.
    new (&runner) Self();

    auto const cpu = mm::CpuLocal::current_cpu();

    for (auto i = usize(0); i < static_cast<usize>(Level::Count); ++i) {
        char name[24];
        ustl::fmt::format_to(name, "Dpc/{}/{}\0", cpu, LEVEL_NAMES[i]);

        [[maybe_unused]]
        auto const status = runner.m_dpc_queues[i].start(name, cpu);
        DEBUG_ASSERT(Status::Ok == status, "Failed to start Dpc worker[{}] on cpu {}",
                     LEVEL_NAMES[i], cpu);
    }
}

auto DpcRunner::enqueue(Dpc &dpc, Level level) -> Status {
    auto const cpu = mm::CpuLocal::current_cpu();
    auto &runner = Current::get();
    runner.m_dpc_queues[static_cast<usize>(level)].enqueue(dpc, cpu, level);
    return Status::Ok;
}

auto DpcRunner::cancel(Dpc &dpc) -> Status {
    auto &runner = Current::get(Self::owner_cpu_of(dpc));
    auto &queue = runner.m_dpc_queues[static_cast<usize>(Self::owner_level_of(dpc))];
    return queue.cancel(dpc);
}

namespace {
    /// This CPU's Dpc workers go live as soon as there is a scheduler to run
    /// them on.
    ///
    /// DpcRunner::init() documents that every CPU must call it on itself once
    /// during its own bring-up -- and nothing ever did. Queue::start() is its
    /// only caller and takes the only address of Queue::do_work(), so with no
    /// caller the linker dropped both: neither symbol existed in the image.
    /// The queues were therefore never started, and Thread::Current::exit()
    /// enqueued the teardown Dpc onto one, which faulted.
    ///
    /// Threading is the first level at which this can run: init() spawns a
    /// thread per Level, and task::init() -- which builds the schedulers and
    /// each CPU's idler -- is what main.cpp finishes immediately before
    /// publishing this level. Registered as a hook rather than called from
    /// main.cpp so the dependency lives next to the code that has it; the
    /// tick's start_legacy_tick() sits on the same level for the same reason.
    ///
    /// This covers the boot CPU only, which is what a hook can do -- hooks run
    /// on whoever publishes the level, and init() resolves Current::get() and
    /// current_cpu() against the running CPU by design. A secondary CPU still
    /// owes itself this call from its own bring-up path.
    INIT_CODE auto start_dpc_runner() -> void {
        DpcRunner::init();
    }
    GKTL_INIT_HOOK(DpcRunnerInit, start_dpc_runner, gktl::InitLevel::Threading);
} // namespace

} // namespace ours
