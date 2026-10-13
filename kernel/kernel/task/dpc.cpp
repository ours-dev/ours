#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/dpc.hpp>
#include <ours/lockdep.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>

#include <new>

#include <arch/auto_interrupt_disabler.hpp>
#include <gktl/init_hook.hpp>
#include <ktl/name.hpp>
#include <ustl/option.hpp>

namespace ours {
namespace {
    constexpr char const *LEVEL_NAMES[static_cast<usize>(DpcPriority::Count)] = {
        "Low",
        "Normal",
        "High",
    };
} // namespace

auto DpcRunner::Queue::start(char const *name, CpuNum cpu) -> Status {
    DEBUG_ASSERT(m_thread == nullptr, "Dpc queue started while it has a worker");
    {
        Guard<sync::SpinLock, IrqSave> guard{m_lock};
        m_stopping = false;
    }
    m_stopped.unsignal();

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
    // detach() fails only for a thread that is already Dead or that a
    // join() has claimed, and one just spawned is neither; resume()
    // answers only Ok.
    [[maybe_unused]]
    auto const detached = m_thread->detach();
    DEBUG_ASSERT(Status::Ok == detached);
    auto _ = m_thread->resume();
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

/// The flag is set and the worker woken in one critical section, the one
/// the worker reads the flag and pops a Dpc in; see do_work() for why that
/// loses no Dpc and runs none twice.
auto DpcRunner::Queue::stop(time::Deadline const deadline) -> Status {
    {
        Guard<sync::SpinLock, IrqSave> guard{m_lock};
        if (m_thread == nullptr) {
            return Status::BadState;
        }
        m_stopping = true;
        m_event.signal();
    }
    if (auto const status = m_stopped.wait(deadline); status != Status::Ok) {
        return status;
    }
    m_thread = nullptr;
    return Status::Ok;
}

/// |from|'s lock, then this queue's. Nothing else holds two queue locks at
/// once, so the order cannot meet its reverse.
auto DpcRunner::Queue::take_over(Queue &from, CpuNum const cpu, Level const level) -> void {
    Guard<sync::SpinLock, IrqSave> from_guard{from.m_lock};
    Guard<sync::SpinLock, IrqSave> guard{m_lock};
    auto moved = false;
    while (!from.m_list.empty()) {
        auto &dpc = from.m_list.front().value();
        from.m_list.pop_front();
        DpcRunner::mark_queued(dpc, cpu, level);
        m_list.push_back(dpc);
        moved = true;
    }
    if (moved) {
        m_event.signal();
    }
}

auto DpcRunner::Queue::do_work() -> i32 {
    while (true) {
        [[maybe_unused]]
        auto const status = m_event.wait();
        DEBUG_ASSERT(Status::Ok == status);

        Dpc *next = nullptr;
        {
            Guard<sync::SpinLock, IrqSave> guard{m_lock};

            // Read where a Dpc is popped, under the same lock as enqueue()
            // and stop(): a Dpc already popped runs, once, on this worker;
            // one still in the list stays there for the next worker, whether
            // it was queued before stop() or after. The event is left
            // signalled, so the next worker looks at the list at once.
            if (m_stopping) {
                break;
            }

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
            // Copying the Dpc onto this worker's stack under the lock and
            // invoking the copy would also leave the original untouched once
            // it leaves the list; no copy is needed, because nothing reads
            // the Dpc after the call.
            //
            // The visible consequence is that State::Running is no longer
            // observable and cancel() of an already-popped Dpc reports
            // NotFound rather than BadState: once a Dpc has left the list it
            // cannot be cancelled.
            DpcRunner::mark_idle(*next);
        }

        next->invoke();
    }

    // The last touch of this queue: stop() may start another worker as soon
    // as it sees this.
    m_stopped.signal();
    return 0;
}

/// Construct |cpu|'s runner before anything touches it. CpuLocal hands out
/// zero-filled storage, and a zeroed Queue is not an empty Queue: its
/// intrusive list header is {null, null} rather than self-pointing, and its
/// Event's canary is 0. Nothing notices until the first enqueue -- do_work()
/// parks on the event without ever inspecting the list, and List::insert()
/// does not consult empty(), it links against the header directly, so
/// link_before() writes through the header's null prev and takes a page
/// fault at the first Dpc ever queued.
///
/// Every other per-CPU object in this tree is built the same way:
/// Scheduler::init() opens with `new (this) Self()`, IdleThread::init() with
/// `new (this) IdleThread(cpunum)`, IpiService::init() with a placement-new
/// per possible CPU.
auto DpcRunner::init(CpuNum cpu) -> void {
    new (&Current::get(cpu)) Self();
}

auto DpcRunner::start_workers(CpuNum cpu) -> void {
    auto &runner = Current::get(cpu);

    for (auto i = usize(0); i < static_cast<usize>(Level::Count); ++i) {
        char buffer[24];
        auto const name = ktl::format_name(buffer, "Dpc/{}/{}", cpu, LEVEL_NAMES[i]);

        [[maybe_unused]]
        auto const status = runner.m_dpc_queues[i].start(name, cpu);
        DEBUG_ASSERT(Status::Ok == status, "Failed to start Dpc worker[{}] on cpu {}",
                     LEVEL_NAMES[i], cpu);
    }
}

/// One level after another: a worker still in a long callback holds up the
/// levels after it, each until |deadline|, and the first failure is kept.
auto DpcRunner::stop_workers(CpuNum const cpu, time::Deadline const deadline) -> Status {
    auto &runner = Current::get(cpu);
    auto status = Status::Ok;
    for (auto &queue: runner.m_dpc_queues) {
        if (auto const stopped = queue.stop(deadline);
            stopped != Status::Ok && status == Status::Ok) {
            status = stopped;
        }
    }
    return status;
}

/// With interrupts off, so this CPU's queues are the ones moved into.
auto DpcRunner::take_over_from(CpuNum const cpu) -> void {
    arch::AutoInterruptDisabler const no_interrupts;
    auto const self = Cpu::id();
    DEBUG_ASSERT(self != cpu, "CPU[{}] taking over its own Dpcs", cpu);
    auto &from = Current::get(cpu);
    auto &to = Current::get();
    for (usize i = 0; i < static_cast<usize>(Level::Count); ++i) {
        to.m_dpc_queues[i].take_over(from.m_dpc_queues[i], self, static_cast<Level>(i));
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
    /// Every possible CPU's runner, and the boot CPU's workers.
    ///
    /// Threading is the first level at which either can run: the workers are
    /// threads, and task::init() -- which builds the schedulers and each CPU's
    /// idler -- is what main.cpp finishes immediately before publishing this
    /// level. Registered as a hook rather than called from main.cpp so the
    /// dependency lives next to the code that has it; the tick's
    /// start_ticks() sits on the same level for the same reason.
    ///
    /// The *storage* is built for every possible CPU here, not just the boot
    /// one, because a Dpc can be enqueued on a CPU long before that CPU has
    /// anything to drain it -- and enqueueing onto zeroed storage faults. This
    /// used to build only the running CPU's, with a comment saying a secondary
    /// still owed itself the call from its own bring-up path; nothing ever
    /// paid it, so the first thread to exit on an AP took a page fault in
    /// DpcRunner::Queue::enqueue().
    INIT_CODE
    auto init_dpc_runners() -> void {
        cpu_possible_mask().for_each([](CpuNum cpu) { DpcRunner::init(cpu); });

        // Only the boot CPU's workers, because it is the only CPU online at
        // this level; the rest follow at Platform, below.
        DpcRunner::start_workers(Cpu::id());
    }
    GKTL_INIT_HOOK(DpcRunnerInit, init_dpc_runners, gktl::InitLevel::Threading);

    /// The secondaries' workers, once x86_bring_aps() has actually started
    /// them. A worker is pinned to its CPU with set_cpu_affinity_hard(), and
    /// resume() places a thread through select_target_cpu(), whose candidate
    /// sets are all subsets of the online mask -- so a worker pinned to a CPU
    /// that is not online yet has nowhere to be put.
    INIT_CODE
    auto start_secondary_dpc_workers() -> void {
        auto secondaries = Cpu::online_mask();
        secondaries.set(Cpu::id(), false);
        secondaries.for_each([](CpuNum cpu) { DpcRunner::start_workers(cpu); });
    }
    GKTL_INIT_HOOK(DpcSecondaryWorkers, start_secondary_dpc_workers, gktl::InitLevel::Platform);
} // namespace

} // namespace ours
