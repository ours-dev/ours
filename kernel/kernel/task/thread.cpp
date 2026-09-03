#include <arch/auto_interrupt_disabler.hpp>
#include <arch/halt.hpp>
#include <gktl/counter.hpp>
#include <gktl/defer.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ours/dpc.hpp>
#include <ours/koid.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/mod.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/idle_thread.hpp>
#include <ours/task/scheduler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer.hpp>
#include <ustl/fmt/format_to.hpp>

namespace {
GKTL_COUNTER(THREAD_SPAWN_COUNT, "thread.spawn");
GKTL_COUNTER(THREAD_JOIN_COUNT, "thread.join");
GKTL_COUNTER(THREAD_RESUME_COUNT, "thread.resume");
GKTL_COUNTER(THREAD_SUSPEND_COUNT, "thread.suspend");
GKTL_COUNTER(THREAD_DESTROY_COUNT, "thread.destroy");
} // namespace

namespace ours::task {
using sync::SpinLock;

using mm::ObjectCache;

using ThreadCache =
    ObjectCache::Instance<Thread,
                          ObjectCache::Option::AddInitHook<"ThreadCache", ObjectCache::Flags::Folio,
                                                           gktl::InitLevel::Heap>>;

/// See the note beside VmaCache in kernel/mm/vm_area.cpp: the option records
/// the level, the namespace-scope hook is the caller's to write, and this one
/// was missing too.
INIT_CODE static auto init_thread_cache() -> void {
    if (Status::Ok != ThreadCache::init_from_option()) {
        panic("Failed to create the object cache for Thread");
    }
    log::trace("ThreadCache has been created");
}
GKTL_INIT_HOOK(ThreadCacheInit, init_thread_cache, gktl::InitLevel::Heap);

/// Related with global thread list
decltype(Thread::SPAWNED_THREADS_LOCK) Thread::SPAWNED_THREADS_LOCK;
decltype(Thread::SPAWNED_THREADS) Thread::SPAWNED_THREADS;

auto to_string(Thread::State state) -> char const * {
    switch (state) {
        case Thread::State::Running:
            return "Running";
        case Thread::State::Ready:
            return "Ready";
        case Thread::State::Dead:
            return "Dead";
        case Thread::State::Suspend:
            return "Suspend";
        case Thread::State::Initial:
            return "Initial";
        case Thread::State::Parked:
            return "Parked";
        case Thread::State::Sleeping:
            return "Sleeping";
        default:
            return "Unknown";
    }
}

FORCE_INLINE auto TaskState::join(Thread *thread, time::Deadline deadline) -> Status {
    return m_result_wait_queue.park_thread(thread, deadline);
}

auto TaskState::try_wake_joiners(Status status) -> bool {
    m_result_wait_queue.notify_all(status);
    return false;
}

Thread::Thread(KoId tid, char const *name, sched::BaseProfile const &profile)
    // Initializer order follows the member declaration order in thread.hpp.
    : m_arch_thread()
    , m_name(name)
    , m_aspace()
    , m_user_thread()
    , m_stack()
    , m_tid(tid)
    , m_resource_lock()
    , m_flags()
    , m_state(State::Initial)
    , m_signals()
    , m_pi_lock()
    , m_scheduling_context(profile)
    , m_preemption_state()
    , m_task_state()
    , m_waiter_state()
    , m_memory_allocation_state()
    , m_owned_wait_queues()
    , m_spawned_hook()
    , m_migrating_hook()
    , m_unpark_hook() {}

auto Thread::create_first(KoId tid, char const *name, sched::BaseProfile const &profile, Self *self)
    -> void {
    DEBUG_ASSERT(self);

    new (self) Self(tid, name, profile);

    // Adopts the stack this code is already running on rather than allocating
    // one. Everything spawn_etc() does past this point -- the task state, the
    // arch context, the scheduler hook -- describes a thread that has yet to
    // start, and none of it applies to one that is already mid-execution.
    self->m_stack.init_first();

    Guard guard{SPAWNED_THREADS_LOCK};
    SPAWNED_THREADS.push_back(*self);
}

auto Thread::spawn_etc(KoId tid, char const *name, StartRoutine entry,
                       sched::BaseProfile const &profile, TrampolineRoutine trampoline_routine,
                       Self *self) -> Status {
    DEBUG_ASSERT(self);

    new (self) Self(tid, name, profile);

    auto const status = self->m_stack.init();
    if (Status::Ok != status) {
        return status;
    }
    self->m_task_state.init(entry);

    if (!trampoline_routine) {
        trampoline_routine = Thread::trampoline;
    }
    self->m_arch_thread.init(VirtAddr(trampoline_routine));

    {
        Guard guard{SPAWNED_THREADS_LOCK};
        SPAWNED_THREADS.push_back(*self);
    }

    // DRANGER!
    // Don't move it. It's inside implementation may be associated with a thread waking event.
    Scheduler::on_thread_init(self, profile);

    THREAD_SPAWN_COUNT.add(1);
    return Status::Ok;
}

auto Thread::spawn(char const *name, StartRoutine entry, sched::BaseProfile const &profile)
    -> Self * {
    // FIXME(SmallHuaZi)
    auto const tid = KoIdGenerator::generate();
    if (KOID_INVALID == tid) {
        return 0;
    }

    // spawn_etc placement-news into this, so take raw storage.
    auto self = ThreadCache::allocate(mm::GAF_KERNEL);
    if (!self) {
        return 0;
    }

    auto const status = Self::spawn_etc(tid, name, entry, profile, 0, self);
    if (Status::Ok != status) {
        ThreadCache::deallocate(self);
        return 0;
    }

    return self;
}

auto Thread::spawn(char const *name, StartRoutine entry, usize priority) -> Self * {
    return Self::spawn(name, entry, sched::BaseProfile(static_cast<i32>(priority)));
}

auto Thread::switch_thread(Self *curr, Self *next) -> void {
    Thread::Current::set(next);
    ArchThread::switch_thread(&curr->m_arch_thread, &next->m_arch_thread);
}

auto Thread::resume() -> Status {
    m_canary.verify<Self>();

    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{get_pi_lock()};
    auto const state = this->state();

    // Do nothing to a dead thread.
    if (state == State::Dead) {
        return Status::Ok;
    }

    m_signals &= ~Signals::Suspend;

    if (state == State::Suspend) {
        Scheduler::unpark_thread(this);
    } else if (state == State::Initial) {
        Scheduler::unpark_thread(this, UnparkControl::Initial);
    }

    THREAD_RESUME_COUNT.add(1);
    return Status::Ok;
}

auto Thread::suspend_or_kill(SuspendOrKillOp op) -> Status {
    m_canary.verify<Self>();

    auto const is_to_suspend = SuspendOrKillOp::Suspend == op;
    auto set_signals = gktl::defer([this, is_to_suspend]() {
        if (is_to_suspend) {
            m_signals |= Signals::Suspend;
        } else {
            m_signals |= Signals::Kill;
        }
    });

    Guard thread_guard{get_pi_lock()};
    auto const curr_state = state();

    // Attempt to kill a thread in dead state, so do nothing. Just get out of here.
    if (curr_state == State::Dead) {
        set_signals.cancel();
        return Status::Ok;
    }

    // Invoke it before to handle the following cases since in the code bottom thread
    // switching may happens.
    set_signals.invoke();

    switch (curr_state) {
        case State::Ready: {
            break;
        }
        case State::Running: {
            break;
        }
        case State::Suspend: {
            if (op == SuspendOrKillOp::Kill) {
                thread_guard.unguard();
                Scheduler::unpark_thread(this);
            }
            break;
        }
        case State::Parked: {
            // The thread is in general **Waiting** state(usually means parked or sleeping).

            // And it is not interruptible so just set the corresponding signal, which will
            // be handle after the thread woken.
            if (!m_waiter_state.is_interruptible()) {
                return Status::Ok;
            }

            auto const wq = m_waiter_state.wait_queue();
            wq->unpark_thread(this);
            break;
        }
        case State::Sleeping: {
            thread_guard.unguard();
            m_waiter_state.unsleep(this, Status());
            break;
        }
        default:
            log::error("Unexpected state {} in {}", to_string(curr_state), __func__);
            break;
    }

    return Status::Ok;
}

auto Thread::join(i32 *retcode, time::Deadline deadline) -> Status {
    m_canary.verify<Self>();
    DEBUG_ASSERT(this == Current::get());

    if (!!(m_flags & Flags::Detached)) {
        return Status::BadState;
    }

    if (state() != State::Dead) {
        auto const status = m_task_state.join(this, deadline);
        if (Status::Ok != status) {
            return status;
        }
    }

    if (retcode) {
        *retcode = m_task_state.retcode();
    }

    destroy();
    THREAD_JOIN_COUNT.add(1);
    return Status::Ok;
}

auto Thread::detach() -> Status {
    m_canary.verify<Self>();

    Guard guard{get_pi_lock()};

    if (state() != State::Dead) {
        m_flags |= Flags::Detached;
        return Status::Ok;
    }

    // The thread has already dead. join() reaps it without ever parking --
    // it only waits when the state is not Dead -- so the deadline below is
    // never consulted; infinite is simply the honest value for "no wait was
    // asked for".
    m_flags &= ~Flags::Detached;
    return join(nullptr, time::Deadline::infinite());
}

auto Thread::set_cpu_affinity_hard(CpuMask const &affinity_mask) -> void {
    m_canary.verify<Self>();
    Scheduler::set_cpu_affinity(this, &affinity_mask, CpuAffinity::Kind::Hard);
}

auto Thread::set_cpu_affinity_soft(CpuMask const &affinity_mask) -> void {
    m_canary.verify<Self>();
    Scheduler::set_cpu_affinity(this, &affinity_mask, CpuAffinity::Kind::Soft);
}

auto Thread::override_base_profile(sched::BaseProfile const &profile) -> void {
    m_canary.verify<Self>();
    return OwnedWaitQueue::override_thread_base_profile(this, profile);
}

auto Thread::bind_user_thread(ustl::Arc<object::ThreadDispatcher> user_thread) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(state() == State::Initial);

    Guard guard{get_resource_lock()};
    m_user_thread = user_thread;

    m_flags |= Flags::Detached;
}

auto Thread::destroy() -> void {
    m_canary.verify<Self>();

    {
        Guard guard{get_resource_lock()};
        Scheduler::on_thread_dead(this);
        set_state_dead();
    }

    this->~Self();
    if (!(m_flags & Flags::NoFree)) {
        ThreadCache::put(this);
    }

    THREAD_DESTROY_COUNT.add(1);
}

auto Thread::trampoline() -> void {
    Scheduler::trampoline_handoff();

    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    auto const retcode = current_thread->m_task_state.invoke();

    Thread::Current::exit(retcode);
}

FORCE_INLINE auto Thread::erase_from_global_list() -> void {
    Guard guard{SPAWNED_THREADS_LOCK};
    return erase_from_global_list_locked();
}

FORCE_INLINE auto Thread::erase_from_global_list_locked() -> void {
    m_canary.verify<Self>();
    (void)SPAWNED_THREADS.erase(SPAWNED_THREADS.iterator_to(*this));
}

auto Thread::Current::preempt() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    // The |IrqSave| option will save current enable/disable state of interrupt,
    // whether it is enabled or disabled now.
    Guard<SpinLock, IrqSave> guard{current_thread->get_pi_lock()};
    Scheduler::Current::preempt(current_thread);
}

auto Thread::Current::reschedule() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    Guard<SpinLock, IrqSave> guard{current_thread->get_pi_lock()};
    Scheduler::Current::reschedule(current_thread);
}

auto Thread::Current::yield() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    Guard<SpinLock, IrqSave> guard{current_thread->get_pi_lock()};
    Scheduler::Current::yield_thread(current_thread);
}

auto Thread::Current::yield_to(Self *next) -> Status {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    Guard<SpinLock, IrqSave> guard{current_thread->get_pi_lock()};
    return Scheduler::Current::yield_to_thread(current_thread, next);
}

auto Thread::Current::migrate_to(CpuNum dest) -> void {
    Thread::Current::get()->set_cpu_affinity_hard(CpuMask::of(dest));
}

INIT_CODE auto Thread::Current::become_idle() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    DEBUG_ASSERT(current_thread->is_idle_thread());
    DEBUG_ASSERT(&IdleThread::Current::get().thread() == current_thread);

    auto const cpunum = Cpu::id();
    {
        char name[24];
        ustl::fmt::format_to(name, "Idle[{}]\0", cpunum);
        Thread::Current::set_name(name);

        Scheduler::Current::set_idling(true);

        // Mark this thread preemptible and wait other one's preemption request.
        current_thread->preemption_state().add_pending();
    }

    arch::local_irq_enable();
    current_thread->preemption_state().enable_preemption();

    IdleThread::Current::run();
    unreachable();
}

auto Thread::Current::sleep_etc(time::Deadline const &deadline, Interruptible interruptible,
                                TimePoint<> now) -> Status {
    using namespace time;

    if (deadline.when() < now) {
        return Status::Ok;
    }

    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();
    DEBUG_ASSERT(current_thread->is_state_running(), "Impossible thread state");

    Timer timer;
    Status woken_status;
    {
        Guard<SpinLock, IrqSave> guard{current_thread->get_pi_lock()};
        DEBUG_ASSERT(current_thread->state() == Thread::State::Running);

        if (Interruptible::Yes == interruptible && current_thread->signals() != Signals::None) {
            /// Throw up to the caller who make decision
            if (current_thread->check_kill_signal()) {
                return Status::InternalIrqKilled;
            }
            return Status::InternalIrqRetry;
        }

        timer.activate(deadline, [current_thread](Timer *timer) {
            current_thread->m_waiter_state.unsleep(current_thread, Status::Ok);
            return Timer::HandlerResult::Handled;
        });

        current_thread->set_state_sleeping();
        current_thread->m_waiter_state.sleep(current_thread, interruptible, Status::ShouldWait);
        woken_status = current_thread->m_waiter_state.status();
    }
    DEBUG_ASSERT(current_thread->state() == State::Running, "Woken ahead");
    timer.cancel();
    return woken_status;
}

auto Thread::Current::sleep_for(Duration delta, Interruptible interruptible) -> Status {
    auto const now = time::SteadyClock::now();
    auto const deadline = time::Deadline::at(now + delta);
    return Thread::Current::sleep_etc(deadline, interruptible, now);
}

auto Thread::Current::sleep_until(time::Deadline const &deadline, Interruptible interruptible)
    -> Status {
    auto const now = time::SteadyClock::now();
    return Thread::Current::sleep_etc(deadline, interruptible, now);
}

auto Thread::Current::do_suspend() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    if (current_thread->m_user_thread) {
        current_thread->m_user_thread->on_suspend();
    }

    auto need_exit = false;
    {
        Guard guard{current_thread->get_pi_lock()};

        if (current_thread->check_kill_signal()) {
            need_exit = true;
        } else if (!!(current_thread->signals() & Signals::Suspend)) {
            // Clear the signal suspending.
            current_thread->m_signals &= ~Signals::Suspend;

            // Mark this thread suspend so rescheduling not to place it
            // onto runqueue again.
            current_thread->set_state_suspend();

            Scheduler::Current::reschedule(current_thread);

            if (current_thread->check_kill_signal()) {
                need_exit = true;
            }
        }
    }

    if (need_exit) {
        Thread::Current::exit(0);
    }

    if (current_thread->m_user_thread) {
        current_thread->m_user_thread->on_resume();
    }
}

auto Thread::Current::process_pending_signals() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    Signals signals;
    while ((signals = current_thread->signals()) != Signals::None) {
        if (!!(signals & Signals::Kill)) {
        }

        if (!!(signals & Signals::Suspend)) {
            arch::local_irq_enable();
            Thread::Current::do_suspend();
            arch::local_irq_disable();
        }

        if (!!(signals & Signals::None)) {
        }
    }
}

/// #1 A kill itself.
///    A send a kill signal to itself and when go back to userspace it will
///    process the signal.
///
/// #2 A kill B.
///    A send a kill signal to B and then B wait either an interrupt or
///    a trap to process the pending kill signal.

auto Thread::Current::kill() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    DEBUG_ASSERT(current_thread->is_idle_thread(), "Try to kill the idle thread");
    current_thread->kill();
}

auto Thread::Current::exit(i32 retcode) -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    log::trace("Thread |{}| is preparing exiting", current_thread->name());

    if (current_thread->m_user_thread) {
        current_thread->m_user_thread->on_exit();
    }

    arch::AutoInterruptDisabler irq_guard;
    AutoPreemptDisabler preempt_guard;

    auto thread_desc_recyclable = false;
    {
        Guard guard{current_thread->get_pi_lock()};
        current_thread->set_state_dead();
        current_thread->m_task_state.set_retcode(retcode);

        auto const flags = current_thread->m_flags;
        if (!!(flags & Flags::Detached)) {
            current_thread->erase_from_global_list();

            if (!!(flags & Flags::NoFree)) {
                thread_desc_recyclable = true;
            }
        } else {
            while (!current_thread->m_task_state.try_wake_joiners(Status::Ok)) {
                arch::halt();
            }
        }

        if (thread_desc_recyclable) {
            Scheduler::Current::reschedule(current_thread);
            unreachable();
        }
    }

    Dpc dpc{&Self::destroy, current_thread};
    [[maybe_unused]]
    auto const status = DpcRunner::enqueue(dpc, DpcRunner::Level::Normal);
    DEBUG_ASSERT(Status::Ok == status);

    // Here we don't hold |current_thread|'s lock, so use Thread::Current::schedule()
    Thread::Current::reschedule();
    unreachable();
}

auto Thread::Current::set_name(const char *name) -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    current_thread->m_name.set(name);
}

template <typename F>
auto Thread::Current::fault(VirtAddr va, F &&f) -> Status {
    if (mm::is_kernel_address(va)) {
        return Status::NotFound;
    }

    auto const self = Thread::Current::get();
    self->m_canary.verify<Self>();

    mm::VmAspace *aspace;
    if (!self->m_user_thread) {
        aspace = self->m_aspace;
    } else {
        aspace = self->m_user_thread->process().aspace();
    }

    return f(aspace);
}

auto Thread::Current::page_fault(VirtAddr va, mm::VmfCause reason) -> Status {
    return Thread::Current::fault(
        va, [va, reason](mm::VmAspace *aspace) { return aspace->page_fault(va, reason); });
}

auto Thread::Current::soft_fault(VirtAddr va, mm::VmfCause reason) -> Status {
    return Thread::Current::fault(
        va, [va, reason](mm::VmAspace *aspace) { return aspace->soft_fault(va, reason); });
}
} // namespace ours::task
