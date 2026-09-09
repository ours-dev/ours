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
INIT_CODE
static auto init_thread_cache() -> void {
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

FORCE_INLINE
auto TaskState::join(Thread *thread, time::Deadline deadline) -> Status {
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

    // Preemption stays off for the whole of the boot thread's life. Anything
    // that flushes a pending preemption before task::init() has built this
    // CPU's scheduler -- and enable_preemption() flushes -- would preempt
    // into a Scheduler that does not exist yet; the first such flush came out
    // of time::init() and died on the scheduler's canary. Zircon's
    // thread_construct_first() does the same PreemptDisable(), and only the
    // idle thread, once it is ready, turns preemption back on for the CPU.
    // Here that is task::init(): the idler it installs as Thread::Current
    // carries its own count, which become_idle() releases.
    self->m_preemption_state.disable_preemption();

    Guard guard{SPAWNED_THREADS_LOCK};
    SPAWNED_THREADS.push_back(*self);
}

auto Thread::spawn_etc(KoId tid, char const *name, StartRoutine entry, void *arg,
                       sched::BaseProfile const &profile, TrampolineRoutine trampoline_routine,
                       Self *self, CpuNum target_cpu) -> Status {
    DEBUG_ASSERT(self);

    new (self) Self(tid, name, profile);

    auto const status = self->m_stack.init();
    if (Status::Ok != status) {
        return status;
    }
    self->m_task_state.init(entry, arg);

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
    Scheduler::on_thread_init(self, profile, target_cpu);

    THREAD_SPAWN_COUNT.add(1);
    return Status::Ok;
}

auto Thread::spawn(char const *name, StartRoutine entry, void *arg,
                   sched::BaseProfile const &profile) -> Self * {
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

    auto const status = Self::spawn_etc(tid, name, entry, arg, profile, 0, self);
    if (Status::Ok != status) {
        ThreadCache::deallocate(self);
        return 0;
    }

    return self;
}

auto Thread::spawn(char const *name, StartRoutine entry, void *arg, usize priority) -> Self * {
    return Self::spawn(name, entry, arg, sched::BaseProfile(static_cast<i32>(priority)));
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
            // |thread_guard| stays armed: unpark_thread() requires the PI lock
            // and does not release it (see its declaration). The unguard()
            // that used to precede this call left the lock held forever on
            // this path -- the next acquire of it from this CPU would trip
            // SpinLock::acquire()'s !is_held() assertion, from anywhere.
            if (op == SuspendOrKillOp::Kill) {
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
            // Same as the Suspend case: unsleep() ends in unpark_thread(),
            // which leaves the PI lock to us.
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

    {
        Guard guard{get_pi_lock()};
        if (!!(m_flags & Flags::Detached)) {
            return Status::BadState;
        }
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
    Scheduler::set_cpu_affinity(this, affinity_mask, CpuAffinity::Kind::Hard);
}

auto Thread::set_cpu_affinity_soft(CpuMask const &affinity_mask) -> void {
    m_canary.verify<Self>();
    Scheduler::set_cpu_affinity(this, affinity_mask, CpuAffinity::Kind::Soft);
}

auto Thread::override_base_profile(sched::BaseProfile const &profile) -> void {
    m_canary.verify<Self>();
    return OwnedWaitQueue::override_thread_base_profile(this, profile);
}

auto Thread::bind_user_thread(ustl::Arc<object::ThreadDispatcher> user_thread) -> void {
    m_canary.verify<Self>();
    DEBUG_ASSERT(state() == State::Initial);

    // The PI lock, because that is what guards m_flags; m_user_thread has
    // no readers that race this (the thread is still Initial).
    Guard guard{get_pi_lock()};
    m_user_thread = user_thread;

    m_flags |= Flags::Detached;
}

auto Thread::destroy() -> void {
    m_canary.verify<Self>();

    // Read before the destructor runs -- m_flags is a member of the object
    // being torn down, and the old code read it after ~Self(). NoFree never
    // changes after publication, but the lock is what the annotation asks
    // for and it costs nothing here.
    auto nofree = false;
    {
        Guard guard{get_pi_lock()};
        nofree = !!(m_flags & Flags::NoFree);
    }

    {
        Guard guard{get_resource_lock()};
        Scheduler::on_thread_dead(this);
        set_state_dead();
    }

    this->~Self();
    if (!nofree) {
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

FORCE_INLINE
auto Thread::erase_from_global_list() -> void {
    Guard guard{SPAWNED_THREADS_LOCK};
    return erase_from_global_list_locked();
}

FORCE_INLINE
auto Thread::erase_from_global_list_locked() -> void {
    m_canary.verify<Self>();
    (void)SPAWNED_THREADS.erase(SPAWNED_THREADS.iterator_to(*this));
}

auto Thread::Current::preempt() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    // Interrupts and preemption both off across reschedule_common(): that is
    // the state it requires, and the PI lock it takes here is the one the
    // switch hands to the incoming thread. Zircon's Preempt() likewise runs
    // under PreemptDisableAndIrqSave.
    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
    Scheduler::Current::preempt(current_thread);
}

auto Thread::Current::reschedule() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    // Whether this context may give up the CPU is decided *before* the guard
    // below changes it: the guard disables interrupts and preemption, so a
    // check made after it would always say "no". It used to live inside
    // Scheduler::Current::reschedule(), behind the guard, and once the guard
    // really disabled interrupts the idle loop asked to reschedule forever
    // and was refused forever. A caller that is not preemptible gets the
    // request recorded instead, to be paid when it enables preemption.
    auto &preemption_state = current_thread->preemption_state();
    if (!preemption_state.is_preemption_enabled() || arch::irqs_disabled()) {
        preemption_state.add_pending();
        return;
    }

    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
    Scheduler::Current::reschedule(current_thread);
}

auto Thread::Current::yield() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
    Scheduler::Current::yield_thread(current_thread);
}

auto Thread::Current::yield_to(Self *next) -> Status {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
    return Scheduler::Current::yield_to_thread(current_thread, next);
}

auto Thread::Current::migrate_to(CpuNum dest) -> void {
    Thread::Current::get()->set_cpu_affinity_hard(CpuMask::of(dest));
}

INIT_CODE
auto Thread::Current::become_idle() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    DEBUG_ASSERT(current_thread->is_idle_thread());
    DEBUG_ASSERT(&IdleThread::Current::get().thread() == current_thread);

    auto const cpunum = Cpu::id();
    {
        // format_to_n, and the terminator written by hand.
        //
        // std::format_to neither bounds its writes nor NUL-terminates, and the
        // trailing "\0" these format strings used to carry did nothing: a
        // format string is taken as a string_view built with strlen, so the
        // extra NUL was cut off before the formatter ever saw it. The buffer
        // was therefore handed to ktl::Name::set(char const *) unterminated,
        // and that calls strlen on it -- an unbounded read off a 24-byte stack
        // buffer, which is why the boot log showed thread names like
        // "Dpc/0/Highal" -- the tail of the previous stack contents showing
        // through -- rather than "Dpc/0/High".
        char name[24];
        auto const end = ustl::fmt::format_to_n(name, sizeof(name) - 1, "Idle[{}]", cpunum).out;
        *end = '\0';
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
        // Preemption off as well as interrupts: this guard is held into
        // reschedule_common() through WaiterState::sleep(), and that is the
        // state reschedule_common() requires and asserts. Zircon's SleepEtc()
        // parks under PreemptDisableAndIrqSaveOption for the same reason.
        // With interrupts alone, the first sleep_for() of the boot -- inside
        // x86_bring_aps() -- died on "Enter reschedule_common() with preempt
        // enabled" the moment Guard began honouring its options.
        Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
        DEBUG_ASSERT(current_thread->state() == Thread::State::Running);

        if (Interruptible::Yes == interruptible && current_thread->signals() != Signals::None) {
            /// Throw up to the caller who make decision
            if (current_thread->check_kill_signal()) {
                return Status::InternalIrqKilled;
            }
            return Status::InternalIrqRetry;
        }

        // The timeout wakes the thread under its PI lock, exactly as
        // WaitQueue::handle_timeout() does: unsleep() requires the lock, and
        // trylock_or_cancel() is how a timer handler may take a lock that the
        // thread cancelling the timer might be holding -- it gives up as soon
        // as cancel() has marked the timer, instead of deadlocking against a
        // canceller that is spinning on this handler to finish. Returns
        // false when the lock was taken (the inverse of a try_acquire()).
        timer.activate(deadline, [current_thread](Timer *timer) {
            auto &pi_lock = current_thread->get_pi_lock();
            if (timer->trylock_or_cancel(pi_lock)) {
                return Timer::HandlerResult::Handled;
            }
            current_thread->m_waiter_state.unsleep(current_thread, Status::Ok);
            pi_lock.release();
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

    // A captureless lambda, so it converts to a plain function pointer and
    // nothing is allocated on the way into the Dpc -- see the note on
    // Dpc::Func. The thread to tear down travels as the Dpc's arg.
    Dpc dpc{[](Dpc *dpc) -> void { dpc->arg<Thread>()->destroy(); }, current_thread};
    [[maybe_unused]]
    auto const status = DpcRunner::enqueue(dpc, DpcRunner::Level::Normal);
    DEBUG_ASSERT(Status::Ok == status);

    // Straight to the scheduler, not through Thread::Current::reschedule().
    //
    // That wrapper is the *voluntary* entry point: it asks whether this
    // context may give up the CPU and, if not, records a pending request and
    // returns. Here it always answered no -- the guards above have interrupts
    // and preemption disabled -- so a dying thread returned from it and ran
    // straight into the unreachable() below. Nothing may follow this call:
    // the thread is already Dead, its Dpc is queued, and there is no state
    // left for it to run on. Scheduler::Current::reschedule() is the
    // unconditional counterpart, and reschedule_common()'s preconditions
    // (interrupts off, preemption off, PI lock held, curr == curr_thread) are
    // exactly what this scope has -- the lock is taken here because the block
    // above dropped it. Zircon ends Thread::Current::Exit() the same way:
    // enqueue the free Dpc, take the thread's lock, RescheduleInternal().
    //
    // panic() rather than unreachable() for what follows: falling through is
    // precisely the defect this replaces, and it cost a silent triple fault
    // to find. Zircon spells it "somehow fell through thread_exit()".
    Guard guard{current_thread->get_pi_lock()};
    Scheduler::Current::reschedule(current_thread);
    panic("Thread |{}| fell through exit()", current_thread->name());
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
