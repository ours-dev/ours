#include <ours/cpu.hpp>
#include <ours/dpc.hpp>
#include <ours/ipi_event.hpp>
#include <ours/ipi_service.hpp>
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

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/auto_interrupt_enabler.hpp>
#include <gktl/counter.hpp>
#include <gktl/defer.hpp>
#include <gktl/init_hook.hpp>
#include <ktl/rwonce.hpp>
#include <logz4/log.hpp>

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
    , m_scheduling_entity(profile)
    , m_preemption_state()
    , m_task_state()
    , m_waiter_state()
    , m_alloc_state()
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
    // of time::init() and died on the scheduler's canary. Only the idle
    // thread, once it is ready, turns preemption back on for the CPU, and it
    // is ready after task::init(): the idler task::init() installs as
    // Thread::Current carries its own count, which become_idle() releases.
    self->m_preemption_state.disable_preemption();

    // IrqSave, as every holder of this lock must: see its declaration.
    Guard<SpinLock, lockdep::IrqSave> guard{SPAWNED_THREADS_LOCK};
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

    // Every thread spawned here is a kernel thread at birth, so it runs on
    // the kernel address space. A user thread reaches ring 3 only after
    // something attaches its process's aspace over this.
    //
    // Naming it explicitly, rather than leaving m_aspace null and letting the
    // context switch fall back, is the point: null then means exactly one
    // thing -- "nobody has attached an address space yet" -- which is a bug
    // rather than a shorthand for the kernel's. spawn_etc() can do this
    // because it already requires the VMM (m_stack.init() above allocates
    // through it); task::init_early()'s create_first() cannot, and attaches
    // later.
    self->m_aspace = mm::VmAspace::kernel_aspace();

    if (!trampoline_routine) {
        trampoline_routine = Thread::trampoline;
    }
    self->m_arch_thread.init(VirtAddr{reinterpret_cast<usize>(trampoline_routine)});

    {
        // Interrupts off, or a tick preempting this section leaves the lock
        // held by a thread parked in this CPU's run queue -- and exit() on
        // this CPU then spins on it with interrupts off, for good. See the
        // lock's declaration.
        Guard<SpinLock, lockdep::IrqSave> guard{SPAWNED_THREADS_LOCK};
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
    auto self = ThreadCache::allocate(mm::ALLOC_KERNEL);
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
    ArchThread::switch_thread(curr, next);
}

auto Thread::resume() -> Status {
    m_canary.verify<Self>();

    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{get_pi_lock()};
    auto const state = this->state();

    // Do nothing to a dead thread.
    if (state == State::Dead) {
        return Status::Ok;
    }

    ktl::write_once(m_signals, m_signals & ~ThreadSignals::Suspend);

    if (state == State::Suspend) {
        Scheduler::unpark_thread(this);
    } else if (state == State::Initial) {
        Scheduler::unpark_thread(this, UnparkOptions::Initial);
    }

    THREAD_RESUME_COUNT.add(1);
    return Status::Ok;
}

auto Thread::suspend_or_kill(SuspendOrKillOp op) -> Status {
    m_canary.verify<Self>();

    auto const is_to_suspend = SuspendOrKillOp::Suspend == op;
    // What a waiter this wakes reads from its wait: the same two answers the
    // wait's own preamble gives for a signal already pending when it parks
    // (check_kill_or_suspend_signal()), so every caller handles them
    // already. With Status::Ok, as it was, the waiter could not tell being
    // interrupted from being given what it waited for.
    auto const wakeup_status = [is_to_suspend] {
        if (is_to_suspend) {
            return Status::InternalIrqRetry;
        }
        return Status::InternalIrqKilled;
    }();
    auto set_signals = gktl::defer([this, is_to_suspend]() {
        // write_once: the target reads its own signals with no lock held, on
        // its way back to ring 3 (process_pending_signals()), and a plain
        // store is one the compiler may tear or defer.
        if (is_to_suspend) {
            ktl::write_once(m_signals, m_signals | ThreadSignals::Suspend);
        } else {
            ktl::write_once(m_signals, m_signals | ThreadSignals::Kill);
        }
    });

    // Interrupts and preemption off for as long as the PI lock is held, as in
    // resume() above. A plain Guard on a SpinLock changes neither, and the
    // thread killing or suspending *itself* is the case that bites: holding
    // its own PI lock, it takes a timer interrupt, and the interrupt's exit
    // path calls Thread::Current::preempt(), which acquires that same lock on
    // the same CPU -- a self-deadlock with nothing else involved.
    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> thread_guard{get_pi_lock()};
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
            // On this CPU, a Running thread is the caller itself -- a thread
            // killing itself -- and it meets the signal on its own way back
            // to ring 3. On another CPU, nothing guarantees it enters the
            // kernel soon: a loop in ring 3 makes no system call, and takes an
            // interrupt only if one happens to be due there. So interrupt it.
            // IpiEvent::Interrupt has an empty handler; what it buys is the
            // interrupt's tail, which runs process_pending_signals() before
            // returning to ring 3. If the IPI finds the thread in the kernel
            // instead, the check on its way out of the kernel acts on it; if
            // it finds interrupts off on that path, it stays pending until
            // SYSRET or IRETQ turns them on, and is taken in ring 3.
            //
            // curr_cpu() cannot change under us: a Running thread leaves its
            // CPU only through a reschedule, every one of which holds its PI
            // lock, and we hold that. Nor can Cpu::id(), with preemption off.
            auto const cpu = curr_cpu();
            DEBUG_ASSERT(cpu != INVALID_CPU_NUM, "Running thread {} belongs to no CPU", name());
            if (cpu != Cpu::id()) {
                IpiService::send_event(cpu, IpiEvent::Interrupt);
            }
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

            // Parked but in no queue: a queue-driven wake has claimed it.
            // Event::signal() dequeues under the queue lock -- which clears
            // the waiter's queue -- and takes this PI lock only afterwards, to
            // unpark it: it is waiting for us, or about to. Leave the wake to it:
            // the signal is set already, and the thread meets it once it
            // runs. WaitQueue::handle_timeout() returns on a null queue for
            // the same reason.
            auto const wq = m_waiter_state.wait_queue();
            if (wq == nullptr) {
                break;
            }
            wq->unpark_thread(this, wakeup_status);
            break;
        }
        case State::Sleeping: {
            // The same rule as a parked thread's: a sleep that is not
            // interruptible ends at its deadline and nowhere else, and the
            // thread meets the signal when it does. sleep() records which
            // kind this is. This branch used to wake every sleeper, and with
            // Status() -- Ok -- so an interrupted sleep read as a full one.
            if (!m_waiter_state.is_interruptible()) {
                return Status::Ok;
            }

            // Same as the Suspend case: unsleep() ends in unpark_thread(),
            // which leaves the PI lock to us.
            m_waiter_state.unsleep(this, wakeup_status);
            break;
        }
        case State::Initial: {
            // Never run: there is no wait to end and no CPU to interrupt, so it
            // is started, and meets the signal where it first heads for ring 3
            // -- ThreadDispatcher::trampoline() runs process_pending_signals()
            // before the first user instruction. A kernel thread has no such
            // point; it meets the signal in its first interruptible wait.
            //
            // Safe to start only because whoever made the thread is done with
            // it: the one caller that reaches an Initial thread is
            // ThreadDispatcher::activate(), which suspends it after saving its
            // entry and binding it. Like resume() above, unpark_thread() needs
            // the PI lock and leaves it held.
            Scheduler::unpark_thread(this, UnparkOptions::Initial);
            break;
        }
        default:
            log::error("Unexpected state {} in {}", to_string(curr_state), __func__);
            break;
    }

    return Status::Ok;
}

/// The joiner side of option B in .agents/tasks/thread-join/DESIGN.md; the
/// contract is on the declaration. The claim (Flags::Joined) stands in for
/// holding this thread's lock across the wait, and TaskState::exited() for
/// asking "is it Dead yet?", so no step holds two threads' PI locks:
///
///  - the claim and its give-back take only this thread's PI lock;
///  - the wait takes only the caller's, inside Event::wait_etc(), which makes
///    "is it signalled?" and the enqueue one step under the event's queue
///    lock, so a signal can neither be missed nor come before the enqueue
///    and be lost;
///  - the reap takes this thread's PI lock (destroy()) and the list lock,
///    one after the other.
auto Thread::join(i32 *retcode, time::Deadline deadline) -> Status {
    m_canary.verify<Self>();
    // A thread joining itself would wait for its own exit.
    DEBUG_ASSERT(this != Thread::Current::get(), "thread |{}| joins itself", name());

    // The claim. Once it is set, only this caller frees the thread: exit()
    // leaves a joinable thread for its reaper, a second join() and a
    // detach() are refused. That is what makes it safe to touch the thread
    // again after the wait -- whether to reap it or to give the claim back.
    {
        Guard<SpinLock, lockdep::IrqSave> guard{get_pi_lock()};
        if (!!(m_flags & (Flags::Detached | Flags::Joined))) {
            return Status::BadState;
        }
        m_flags |= Flags::Joined;
    }

    // Not interruptible: a join ends at its deadline or at the exit, not at
    // a signal to the joiner. Ok means the thread is off its stack; with the
    // event latched, a join after the exit returns Ok at once, however late
    // its deadline.
    auto const status = m_task_state.exited().wait(deadline, Interruptible::No);
    if (Status::Ok != status) {
        // TimeOut: give the claim back and leave the thread as it was. It
        // may have exited since the wait gave up; then it stays Dead and
        // joinable, and the next join() or detach() reaps it.
        Guard<SpinLock, lockdep::IrqSave> guard{get_pi_lock()};
        m_flags &= ~Flags::Joined;
        return status;
    }

    // Written under the PI lock by exit(), published by the event's lock.
    if (retcode != nullptr) {
        *retcode = m_task_state.retcode();
    }

    // A joinable thread stays on the global list until its reaper takes it
    // off, and destroy() does not.
    erase_from_global_list();
    destroy();

    THREAD_JOIN_COUNT.add(1);
    return Status::Ok;
}

auto Thread::detach() -> Status {
    m_canary.verify<Self>();

    {
        Guard<SpinLock, lockdep::IrqSave> guard{get_pi_lock()};

        // A join() has the claim, and with it the duty to free the thread.
        // Waking the joiner with BadState instead would mean signalling an
        // event the thread, once detached, may free with itself, or racing a
        // timed-out joiner that is handing its claim back. So the detach is
        // refused (the user's call, 2026-10-04).
        if (!!(m_flags & Flags::Joined)) {
            return Status::BadState;
        }

        if (state() != State::Dead) {
            m_flags |= Flags::Detached;
            return Status::Ok;
        }
    }

    // Dead and not detached, so nobody else will free it: reap it here. Not
    // under the PI lock, as the old body did -- join() takes that lock itself,
    // and waits.
    // join() may still wait, briefly: a thread is Dead before it is off its
    // stack, and exited() is signalled only after.
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
    Guard<SpinLock, lockdep::IrqSave> guard{get_pi_lock()};
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
        Guard<SpinLock, lockdep::IrqSave> guard{get_pi_lock()};
        nofree = !!(m_flags & Flags::NoFree);
    }

    {
        Guard<SpinLock, lockdep::IrqSave> guard{get_resource_lock()};
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
    // exit() calls this with interrupts already off, where saving and
    // restoring them is a no-op; the option is for the rule, not for this
    // caller -- see the lock's declaration.
    Guard<SpinLock, lockdep::IrqSave> guard{SPAWNED_THREADS_LOCK};
    return erase_from_global_list_locked();
}

FORCE_INLINE
auto Thread::erase_from_global_list_locked() -> void {
    m_canary.verify<Self>();
    auto _ = SPAWNED_THREADS.erase(SPAWNED_THREADS.iterator_to(*this));
}

auto Thread::Current::preempt() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    // Interrupts and preemption both off across reschedule_common(): that is
    // the state it requires, and the PI lock it takes here is the one the
    // switch hands to the incoming thread.
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

    // No renaming here. IdleThread::init() already named this thread after the
    // CPU it belongs to, and it is the only place that can name the idlers of
    // CPUs that are not running yet -- task::init() builds all of them from the
    // boot CPU. Formatting a second, differently-spelled name ("Idle[{}]" over
    // "Idle/{}") on the way into the idle loop overwrote a correct name with an
    // equivalent one and gave the convention two spellings to keep in sync.
    Scheduler::Current::set_idling(true);

    // Mark this thread preemptible and wait other one's preemption request.
    current_thread->preemption_state().add_pending();

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
        // state reschedule_common() requires and asserts. With interrupts
        // alone, the first sleep_for() of the boot -- inside x86_bring_aps()
        // -- died on "Enter reschedule_common() with preempt enabled" the
        // moment Guard began honouring its options.
        Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
        DEBUG_ASSERT(current_thread->state() == Thread::State::Running);

        if (Interruptible::Yes == interruptible &&
            current_thread->signals() != ThreadSignals::None) {
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
            // Winning the lock does not mean the thread still sleeps. A
            // suspend or kill may have woken it early (suspend_or_kill()'s
            // Sleeping branch); it then stays armed until the thread runs
            // and reaches timer.cancel() below, and a thread woken onto a
            // busy CPU can wait a whole time slice for that. Unsleeping it
            // again would put a Ready or Running thread back on a run queue.
            if (current_thread->is_state_sleeping()) {
                current_thread->m_waiter_state.unsleep(current_thread, Status::Ok);
            }
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
        // ThreadDispatcher::on_suspend() answers only Ok.
        auto _ = current_thread->m_user_thread->on_suspend();
    }

    auto need_exit = false;
    {
        // Interrupts and preemption both off: the guard is carried into
        // reschedule_common() through Scheduler::Current::reschedule() below,
        // and that is the state it requires, exactly as for
        // Thread::Current::reschedule() and yield(). When this thread is
        // resumed, reschedule() returns with the PI lock held again, and the
        // guard's release at the end of this scope restores the interrupt and
        // preemption state process_pending_signals() entered with -- both on,
        // which is what Thread::Current::exit() below asserts.
        Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};

        if (current_thread->check_kill_signal()) {
            need_exit = true;
        } else if (!!(current_thread->signals() & ThreadSignals::Suspend)) {
            // Clear the signal suspending.
            ktl::write_once(current_thread->m_signals,
                            current_thread->m_signals & ~ThreadSignals::Suspend);

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
        // ThreadDispatcher::on_resume() answers only Ok.
        auto _ = current_thread->m_user_thread->on_resume();
    }
}

auto Thread::Current::process_pending_signals() -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    // Called on the last stretch back to ring 3 -- the exception tail, the
    // path to SYSRET, and a thread's first entry -- with interrupts off, so
    // that nothing on this CPU can raise a signal between this check and the
    // return. A signal raised on another CPU in that window is not missed
    // either: suspend_or_kill() follows it with an interrupt IPI to this CPU,
    // which stays pending while interrupts are off and is taken the moment
    // the return to ring 3 turns them on -- and that interrupt's tail comes
    // back here.
    // Each branch below turns them on for what it does and, if it comes back
    // at all, off again.
    DEBUG_ASSERT(arch::irqs_disabled(), "{}() entered with interrupts on", __func__);

    ThreadSignals signals;
    while ((signals = current_thread->signals()) != ThreadSignals::None) {
        // Kill first, and for good: a thread that is both suspended and
        // killed has nothing to resume into. Exiting needs interrupts on --
        // the ThreadDispatcher's on_exit() takes sleeping locks -- and does
        // not come back. This branch used to be empty, and a pending Kill
        // made this loop spin for ever with interrupts off.
        if (!!(signals & ThreadSignals::Kill)) {
            arch::local_irq_enable();
            Thread::Current::exit(0);
        }

        if (!!(signals & ThreadSignals::Suspend)) {
            {
                arch::AutoInterruptEnabler const irqs_on;
                Thread::Current::do_suspend();
            }
            continue;
        }

        // After Kill and Suspend: a killed thread has no stack worth
        // sampling, and a suspended one is sampled once resumed, when the
        // session may have stopped -- the sampler then refuses the record.
        // Cleared before the walk, under the lock every writer of the bits
        // holds, so a request the walk itself takes a tick for is not lost.
        if (!!(signals & ThreadSignals::SampleStack)) {
            auto request = sampler::StackSampleRequest{};
            {
                Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{
                    current_thread->get_pi_lock()};
                ktl::write_once(current_thread->m_signals,
                                current_thread->m_signals & ~ThreadSignals::SampleStack);
                request = current_thread->m_stack_sample;
            }
            // Reading user memory can fault, so the walk runs with
            // interrupts on; the sampler turns them off again around its
            // check that the session runs and its write.
            arch::AutoInterruptEnabler const irqs_on;
            request.record();
            continue;
        }

        // A bit neither branch handles would never be cleared, and the loop
        // would spin on it for ever -- with interrupts off, so silently.
        panic("thread {} has signals {:#x} nobody handles", current_thread->name(),
              static_cast<u32>(signals));
    }
}

auto Thread::Current::request_stack_sample(sampler::StackSampleRequest const request) -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_stack_sample = request;

    // Under the PI lock, as every writer of the signal bits is: another CPU
    // may be raising Kill or Suspend on this thread at the same moment. This
    // CPU cannot hold it already -- the tick interrupted user space -- and
    // every holder keeps interrupts off, so the interrupt never meets it here.
    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
    ktl::write_once(current_thread->m_signals,
                    current_thread->m_signals | ThreadSignals::SampleStack);
}

auto Thread::Current::on_user_exception(ExceptionContext const &context) -> ExceptionOutcome {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();
    DEBUG_ASSERT(!arch::irqs_disabled(), "{}() entered with interrupts off", __func__);

    // Only a user thread executes in ring 3, and only a user thread has a
    // ThreadDispatcher; an exception from ring 3 on a kernel thread would
    // mean the frame lied about where it came from.
    ASSERT(current_thread->m_user_thread, "a user exception on kernel thread {}",
           current_thread->name());
    return current_thread->m_user_thread->on_user_exception(context);
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

    // The idle thread is the one thread that must never die: it is what a
    // CPU runs when nothing else can. The message always said so; the
    // condition said the opposite, which demanded the idle thread and
    // refused every other one.
    DEBUG_ASSERT(!current_thread->is_idle_thread(), "Try to kill the idle thread");
    // suspend_or_kill() answers only Ok.
    auto _ = current_thread->kill();
}

auto Thread::Current::exit(i32 retcode) -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    // The dying thread still has work that may block: its ThreadDispatcher's
    // on_exit() below takes sleeping locks. So it has to arrive here able to
    // block -- interrupts on, preemption on -- which is what every caller
    // promises: the trampoline after the thread's entry returns, and the two
    // signal paths, which turn interrupts back on before calling.
    //
    // Not asserted: that no spin lock is held. SpinLock::num_held_cpu() exists,
    // but the PI lock is handed across a context switch, and whether that
    // per-CPU count balances has not been checked.
    DEBUG_ASSERT(!arch::irqs_disabled(), "{}() entered with interrupts off", __func__);
    DEBUG_ASSERT(Thread::Current::preemption_state().is_preemption_enabled(),
                 "{}() entered with preemption off", __func__);

    log::trace("Thread |{}| is preparing exiting", current_thread->name());

    if (current_thread->m_user_thread) {
        // ThreadDispatcher::on_exit() answers ProcessDispatcher::detach_thread()'s
        // Ok, the only answer it has.
        auto _ = current_thread->m_user_thread->on_exit();
    }

    arch::AutoInterruptDisabler irq_guard;
    AutoPreemptDisabler preempt_guard;

    // How the descriptor ends. The three are exclusive, which is why this is
    // one value and not two flags, and are decided under the PI lock below.
    enum class Ending : u8 {
        // Detached and NoFree: a CPU's idle thread, whose descriptor is not
        // from ThreadCache. It switches away for good; nothing frees it.
        Kept,
        // Detached: off the global list now; the Dpc below frees it once it
        // is off its stack.
        SelfFreed,
        // Not detached: stays on SPAWNED_THREADS as a zombie, whether or not
        // a join() has claimed it yet. The Dpc below tells its reaper --
        // join(), or detach() after the fact -- that it is off its stack, and
        // the reaper unlinks and frees it.
        Zombie,
    };

    auto ending = Ending::Zombie;
    {
        // The disablers above already hold interrupts and preemption off, and
        // this guard says the same of itself: it is carried into
        // reschedule_common() on the Kept path, which needs both.
        Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
        current_thread->set_state_dead();
        current_thread->m_task_state.set_retcode(retcode);

        auto const flags = current_thread->m_flags;
        if (!!(flags & Flags::Detached)) {
            current_thread->erase_from_global_list();
            ending = Ending::SelfFreed;
            if (!!(flags & Flags::NoFree)) {
                ending = Ending::Kept;
            }
        }
        // A Zombie gets no free Dpc: its reaper frees it. The reaper needs to
        // know when that is safe, and Dead is too early: this thread runs on
        // its stack until the reschedule below. The Dpc queued below tells
        // it, from after the switch.
        //
        // The Zombie path panicked while join() was missing, and before that
        // looped `while (!try_wake_joiners(Status::Ok)) arch::halt();` --
        // interrupts off, this PI lock held, on a function that always
        // returned false. Waking the joiners here, under this lock, would take
        // each joiner's PI lock inside this one, and nothing orders two PI
        // locks without a chain-lock backoff
        // (.agents/tasks/thread-join/DESIGN.md, option B).

        if (ending == Ending::Kept) {
            Scheduler::Current::reschedule(current_thread);
            unreachable();
        }
    }

    // Captureless lambdas, so they convert to plain function pointers and
    // nothing is allocated on the way into the Dpc -- see the note on
    // Dpc::Func. The thread travels as the Dpc's arg.
    //
    // Either Dpc runs after this thread's final switch, never before: its
    // worker is pinned to this CPU (DpcRunner::start_workers()), and this
    // thread keeps preemption off from here until the reschedule below. Both
    // are also the last code to see the Dpc, which lives on the stack they
    // let go of: DpcRunner is done with a Dpc before it invokes it.
    //
    // A SelfFreed thread is freed. A Zombie tells its reaper that it is off
    // its stack, and is freed by the reaper.
    auto func = Dpc::Func{[](Dpc *dpc) -> void { dpc->arg<Thread>()->destroy(); }};
    if (ending == Ending::Zombie) {
        func = [](Dpc *dpc) -> void {
            auto *const thread = dpc->arg<Thread>();
            {
                // The PI lock is free only once the switch away from
                // |thread| is complete: the next thread releases it in
                // Scheduler::complete_context_switch(). The pinning above
                // already orders this after that, so this is the check that
                // it did, not a wait.
                Guard<SpinLock, lockdep::IrqSave> guard{thread->get_pi_lock()};
                DEBUG_ASSERT(thread->state() == State::Dead);
            }
            // The last touch of |thread| from here: once signalled, its
            // reaper may free it, this Dpc's storage with it.
            thread->m_task_state.exited().signal(Status::Ok);
        };
    }
    Dpc dpc{func, current_thread};
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
    // above dropped it.
    //
    // panic() rather than unreachable() for what follows: falling through is
    // precisely the defect this replaces, and it cost a silent triple fault
    // to find.
    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
    Scheduler::Current::reschedule(current_thread);
    panic("Thread |{}| fell through exit()", current_thread->name());
}

auto Thread::Current::set_name(const char *name) -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Self>();

    // set() refuses only a null name, which it records as empty; a long one
    // is cut short.
    auto _ = current_thread->m_name.set(name);
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

/// A fault on a page that has to be waited for -- one a page source has not
/// supplied -- can end its wait before the page comes: a suspend ends it with
/// InternalIrqRetry, so the thread can stop, and a kill with
/// InternalIrqKilled. For a fault taken in ring 3 neither is the fault
/// failing. It answers Ok: the handler returns, the thread stops or dies on
/// its way back to ring 3 (process_pending_signals() in the exception's
/// tail), and a thread that is resumed runs the faulting instruction again,
/// which faults again. Answered as they are, either would be a fatal page
/// fault, and a suspend would end the process.
///
/// A fault taken in ring 0 -- a copy to or from user memory -- waits without
/// letting a suspend end it (VmAspace::page_fault_internal() passes whether
/// the fault is ring 3's), so it never sees InternalIrqRetry; answered Ok it
/// would send the copy straight back into a wait that ends at once while the
/// signal is pending. A kill may end it, and is answered as it is: the copy
/// fails, and the thread dies at the end of its system call.
auto Thread::Current::page_fault(VirtAddr va, mm::VmFault::Cause reason) -> Status {
    auto const status = Thread::Current::fault(
        va, [va, reason](mm::VmAspace *aspace) { return aspace->page_fault(va, reason); });

    if (!(reason & mm::VmFault::Cause::User)) {
        DEBUG_ASSERT(Status::InternalIrqRetry != status,
                     "a fault taken in ring 0 had its wait ended by a suspend");
        return status;
    }
    if (Status::InternalIrqRetry == status || Status::InternalIrqKilled == status) {
        return Status::Ok;
    }
    return status;
}

auto Thread::Current::soft_fault(VirtAddr va, mm::VmFault::Cause reason) -> Status {
    return Thread::Current::fault(
        va, [va, reason](mm::VmAspace *aspace) { return aspace->soft_fault(va, reason); });
}
} // namespace ours::task
