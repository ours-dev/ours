/// Placing a thread on a scheduler goes through two layers, both of which
/// require this scheduler's runqueue lock (`get_lock()`) to be held:
///
///     activate_thread / deactivate_thread  -- residency: which CPU owns it
///     enqueue_thread  / dequeue_thread     -- queueing:  where it sits there
///
/// [enqueue_thread / dequeue_thread]
///
/// The queueing layer. It updates the scheduling clock (unless SkipClock),
/// delegates the actual insertion/removal to the thread's own discipline via
/// `ISchedule::enqueue_thread`/`dequeue_thread`, and -- only once the
/// discipline reports Status::Ok -- maintains this scheduler's aggregate
/// accounting, `m_run_queue.m_num_queued_threads` and `m_run_queue.m_total_weight`.
///
/// The discipline's Status is returned verbatim, and any non-Ok result means
/// nothing was accounted for. In particular Status::Delayed (see
/// Fair::try_to_delay_dequeue, gated on SCHED_DELAY_DEQUEUE) reports that an
/// ineligible sleeper was deliberately left in place: it is still queued, and
/// the counters intentionally stay put.
///
/// This layer touches neither half of RunQueueState. TransientState belongs to
/// the residency layer below, and Disposition belongs to the discipline, which
/// calls `RunQueueState::on_enqueue()`/`on_dequeue()` itself.
///
/// Call this layer directly only to re-place a thread on the CPU it already
/// lives on, with no change of residency: Scheduler::Pi re-sorts a thread whose
/// effective profile changed by pairing `Save | SkipClock` with
/// `Restore | SkipClock` (see scheduler-pi.cpp).
///
/// [activate_thread / deactivate_thread]
///
/// The residency layer: the above, plus the TransientState machine that tracks
/// a thread while it is between runqueues.
///
///     deactivate_thread  marks the thread Migrating, then dequeues it. Once it
///                        returns, the thread is on no runqueue and this
///                        scheduler's lock may be dropped.
///     activate_thread    sees a thread that arrives still marked Migrating and
///                        adds EnqueueOptions::Migrated, so the discipline can
///                        distinguish a completed migration from a fresh
///                        wakeup; then enqueues it and marks it Queueing.
///
/// The two are designed to be used as a pair spanning two schedulers -- see
/// Scheduler::Migrator::move_queued_thread (scheduler-migrator.cpp), which
/// deactivates under the old CPU's lock, hands the thread across, and activates
/// under the new CPU's. Migrating is exactly that window. activate_thread is
/// also used alone, by do_unpark_thread, to admit a waking thread.
///
/// Migrating therefore means "runnable, in flight between runqueues", never
/// merely "off the runqueue" -- a blocking thread leaves via the plain
/// dequeue_thread path in reschedule_common and becomes None, not Migrating.
/// Marking a sleeper Migrating would both promise observers an arrival that
/// never comes, and make the later activate_thread tag a plain wakeup with
/// EnqueueOptions::Migrated. deactivate_thread asserts this distinction with
/// `DEBUG_ASSERT(thread->is_state_sleeping() == false)`.
///
/// Both return void, and assert in a debug build that the Status underneath
/// them is Ok. No schedule refuses an enqueue. A dequeue is delayed
/// (Status::Delayed) only for a sleeper, with DequeueOptions::Sleep, and
/// deactivate_thread's callers never pass it: a delayed one would leave a
/// thread marked Migrating while it is in fact still queued.

#include <ours/cpu.hpp>
#include <ours/ipi_service.hpp>
#include <ours/lockdep.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/sched/features.hpp>
#include <ours/task/sched/sched_entity.hpp>
#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/scheduler.hpp>
#include <ours/task/sched/scheduler_inline.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer_queue.hpp>

#include <atomic>
#include <utility>

#include <arch/auto_interrupt_disabler.hpp>
#include <gktl/counter.hpp>
#include <heap/new.hpp>
#include <ktl/rwonce.hpp>

using ours::sync::SpinLock;

namespace ours::task::sched {
namespace {
    extern "C" ScheduleEntry const SCHEDULES_START[] LINK_NAME("__schedules_start");
    extern "C" ScheduleEntry const SCHEDULES_END[] LINK_NAME("__schedules_end");

    GKTL_COUNTER(SCHEDULE_IDLE_PAST, "schedule.idle_past");
    GKTL_COUNTER(SCHEDULE_PREEMPT_PAST, "schedule.preempt_past");
    GKTL_COUNTER(SCHEDULE_YIELD_PAST, "schedule.yield_past");

} // namespace

auto Scheduler::trampoline_handoff() -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();

    auto const current_thread = Thread::Current::get();
    // Release the two locks handed across the switch -- the previous thread's
    // PI lock (finish_switch_context) and this thread's own, taken by whoever
    // unparked it -- *before* interrupts come back on. With interrupts enabled
    // first, a tick arriving in the gap runs Scheduler::tick_thread() against
    // this thread, which takes its PI lock, finds it already held by this CPU
    // and dies on `!is_held()`. That was the first tick of the boot. Zircon's
    // thread trampoline does the same: LockHandoff, then arch_enable_ints().
    Self::complete_context_switch(current_thread);
    current_thread->get_pi_lock().release();

    // And then on, here, explicitly -- nothing else will do it. A thread that
    // is switched back *in* returns through reschedule_common() into its
    // caller, whose Guard<..., IrqSave> restores the interrupt state that
    // thread saved. A thread running for the first time returns into nothing:
    // it starts here, with no Guard of its own on its stack, and
    // x86_switch_context() does not carry RFLAGS across. So it arrives with
    // interrupts off, and without this line it would stay that way for life
    // -- every later park saves that 0 and restores it. Measured: every
    // kernel thread (DPC workers, kshell, laborer, the sample threads) started
    // with IF=0, and a sample thread printed 71080 times on cpu2 with
    // interrupts off throughout, so that CPU took no tick and no IPI.
    arch::local_irq_enable();
}

auto Scheduler::on_thread_init(Thread *thread, BaseProfile const &profile, CpuNum target_cpu)
    -> void {
    auto &context = thread->scheduling_entity();

    {
        Guard<SpinLock, IrqSave> thread_guard{thread->get_pi_lock()};

        // Affinity first, CPU second, in both arms. select_target_cpu()
        // filters its candidates through CpuAffinity::get_available_mask(),
        // so the affinity has to be installed before it runs -- when these
        // two steps were the other way round the selection intersected
        // against an affinity that did not exist yet.
        //
        // The lock spans both steps for the same reason they are ordered: the
        // affinity that decides the answer must not change while the answer is
        // being computed. Nothing inside takes a further lock (the
        // discipline's select_target_cpu is a pure query over per-CPU
        // runtimes), so this introduces no ordering against get_lock().
        if (INVALID_CPU_NUM == target_cpu) {
            // Shared, not copied: a thread that may run "wherever is online"
            // has to keep following the online mask as the secondaries come
            // up. A copy would freeze it at the CPUs online when the thread
            // was created, which on the boot path is the boot CPU alone.
            context.m_affinity.set_affinity_hard_shared(Cpu::online_mask_live());
            target_cpu = select_target_cpu(thread);
        } else {
            // A caller-pinned thread. The pin is recorded as the affinity and
            // not merely honoured here, so that everything downstream -- the
            // re-selection do_unpark_thread() performs, a future migration --
            // reaches the same conclusion from the thread's own state instead
            // of from a decision only this function saw.
            //
            // Selection is skipped rather than being fed a one-CPU affinity
            // and trusted to agree, because it cannot: both of its candidate
            // sets are subsets of the online mask, and task::init() pins each
            // idler to its CPU while walking cpu_possible_mask(), before any
            // secondary CPU is online. Intersecting {cpunum} with an online
            // mask that does not yet contain it is empty, and empty is the
            // one answer select_target_cpu() must not give.
            // Copied rather than shared: a one-CPU mask has nothing to
            // follow, and the copy is what makes a mask built right here at
            // the call site safe to install.
            context.m_affinity.set_affinity_hard(CpuMask::of(target_cpu));
        }

        auto &self = Scheduler::Current::get(target_cpu);
        self.m_canary.verify<Self>();

        // Bind the thread to its discipline first. This is the moment that
        // binding belongs to -- the thread has just been created and has never
        // been near a scheduler -- and until it happens the context's
        // m_schedule is null. Reading it straight out, which is what this did,
        // called through that null pointer: the CPU branched to address 0 and
        // took an instruction-fetch fault there.
        //
        // set_schedule() also puts the lookup behind get_schedule()'s
        // assertion, so a discipline with no registered ISchedule now says so
        // by name instead of producing a wild call.
        auto *const schedule = self.set_schedule(thread);
        schedule->on_thread_init(self.m_run_queue, thread);
    }
}

auto Scheduler::init_first_thread(Thread *thread, CpuNum cpunum) -> void {
    DEBUG_ASSERT(thread);

    auto &self = Scheduler::Current::get(cpunum);
    self.m_canary.verify<Self>();

    auto &context = thread->scheduling_entity();
    Guard<SpinLock, IrqSave> thread_guard{thread->get_pi_lock()};

    // The thread is where it is and stays there: hard-pinned to |cpunum| the
    // way on_thread_init() pins an idler, and recorded as having last run
    // there so that every later "where was it" question answers this CPU.
    context.m_affinity.set_affinity_hard(CpuMask::of(cpunum));
    context.m_curr_cpu = cpunum;
    context.m_last_cpu = cpunum;

    // Bind it to its discipline exactly as on_thread_init() would; a thread
    // that is already running is still a thread the discipline must know.
    auto *const schedule = self.set_schedule(thread);
    schedule->on_thread_init(self.m_run_queue, thread);

    // Now the part no other path provides. The switch that would normally
    // install a current thread has not happened and never will for this one:
    // it is the thread the CPU booted on, or was handed by task::init(). So it
    // is installed directly, with its slice clock started now, and named as
    // the idler this scheduler falls back to when it runs out of work -- the
    // seat evaluate_next_thread() and put_prev_thread()/set_next_thread()
    // consult through m_idle_thread, which nothing else ever wrote.
    self.m_run_queue.m_curr_thread = thread;
    self.m_run_queue.m_curr_started_time = self.m_run_queue.m_clock_source.update_clock();
    if (thread->is_idle_thread()) {
        self.m_idle_thread = thread;
    }
}

auto Scheduler::on_thread_dead(Thread *thread) -> void {
    [[maybe_unused]]
    auto &ctx = thread->scheduling_entity();
    // se.schedule()->on_thread_dead(m_run_queue, thread);
}

/// Unpark don't really cause entry into reschedule routine. It just
/// add a task to the run-queue.
///
/// If the new task added to the run-queue preempts the current task,
/// then the Scheduler::unpark_preempt will call PreemptionState::add_pending()
/// to set preemption flags reschedule() gets called on the nearest possible
/// occasion.
auto Scheduler::unpark_thread(Thread *thread, UnparkOptions options) -> void {
    DEBUG_ASSERT(thread != Thread::Current::get());

    /// Assign CPU for passed |thread|
    CpuNum target_cpu;
    if (!!(options & UnparkOptions::ThisCpu)) {
        target_cpu = Cpu::id();
    } else {
        target_cpu = select_target_cpu(thread);
    }

    auto &self = Scheduler::Current::get(target_cpu);
    self.m_canary.verify<Self>();

    self.do_unpark_thread(thread, options);

    reschedule_mask(CpuMask::of(target_cpu));
}

auto Scheduler::unpark_thread(Thread::UnparkList threads, UnparkOptions options) -> void {
    CpuMask to_resched;
    while (!threads.empty()) {
        auto *const thread = &threads.front().value();
        threads.pop_front();

        // <Thread>, not <Self>: inside Scheduler, Self is Scheduler, and a
        // canary seeded with magic<Thread>() never equals magic<Scheduler>().
        // Three call sites in this file verified a Thread with Scheduler's
        // magic (here, reschedule_mask(), Current::tick()), so the first
        // unpark of the boot died in the check rather than in anything real.
        thread->m_canary.verify<Thread>();
        // IrqSave only: the reschedule this may call for is collected in
        // |to_resched| and requested after the loop, once no PI lock is held.
        Guard<SpinLock, IrqSave> thread_guard{thread->get_pi_lock()};

        CpuNum target_cpu;
        if (!!(options & UnparkOptions::ThisCpu)) {
            target_cpu = Cpu::id();
        } else {
            target_cpu = select_target_cpu(thread);
        }

        auto &self = Self::Current::get(target_cpu);
        self.m_canary.verify<Self>();

        self.do_unpark_thread(thread, options);

        to_resched.set(target_cpu);
    }

    reschedule_mask(to_resched);
}

auto Scheduler::reschedule(CpuNum cpu) -> void {
    if (cpu == Cpu::id()) {
    }
}

auto Scheduler::reschedule_mask(CpuMask const &mask) -> void {
    auto const cpu = Cpu::id();
    auto const need_preempt = mask.test(cpu);

    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    auto &preemption_state = current_thread->preemption_state();
    if (preemption_state.eager_resched_disabled_count() == 0) {
        // The eager reschedule is enabled, so let's trigger the remote scheduler
        // to handle them.
        IpiService::send_resched(mask);
    } else {
        // Disabled, delay it until reaching the nearest preemption occasion.
        preemption_state.add_pending(mask);
    }

    if (!need_preempt) {
        return;
    }

    // The local CPU's preemption is only ever *requested* here, never
    // performed. Every caller reaches this with a lock held that a switch
    // would trip over -- Thread::resume() holds the woken thread's PI lock,
    // which switch_context() is about to take for itself -- and the tick's
    // timer callbacks reach it from interrupt context, where a switch would
    // leave the interrupt half-handled. Zircon's PreemptLocked() defers for
    // the first reason (spinlocks held) and RescheduleMask() for the second.
    // The request is paid at the next preemption point: enable_preemption()
    // flushes it, and the idle loop re-evaluates the run queue on its own.
    preemption_state.add_pending();
}

/// |cpunum| is handed in rather than read from Cpu::id(), for the same reason
/// IdleThread::init() takes one: task::init() walks cpu_possible_mask() and
/// builds every CPU's scheduler from the boot CPU, long before the secondaries
/// exist. Asking the running CPU there answered 0 for all of them.
///
/// which_cpu is not bookkeeping. It is the bit set_state() flips, so every
/// AP's Scheduler::Current::set_active(true) was setting CPU 0's bit in the
/// Active mask and leaving its own clear -- select_target_cpu() consults that
/// mask first, so no AP could ever be handed a thread. And it is what
/// reschedule_common() checks itself against, which is where the boot
/// eventually stopped: "Invoke reschedule_common() on mismatched CPU[1]".
INIT_CODE
auto Scheduler::init(CpuNum cpunum) -> void {
    new (this) Self();

    auto const num_schedule = SCHEDULES_END - SCHEDULES_START;
    DEBUG_ASSERT(num_schedule > 0);

    // Two registrations claiming one policy slot is a silent overwrite: the
    // array is indexed by Discipline, so the second entry replaces the first
    // and the losing discipline simply never runs, with nothing said. Today
    // both Fair and RoundRobin register Discipline::Normal and they are kept
    // apart by a comment in BUILD.gn -- link them together and this is what
    // happens.
    //
    // Linux does not need the check because it does not have the collapse:
    // DEFINE_SCHED_CLASS gives every class its own slot in the linker script,
    // so uniqueness is structural. Flattening that into an array indexed by an
    // enum is what moves the obligation here.
    //
    // A plain assertion is enough despite init() running once per CPU from the
    // BSP: the placement-new above value-initialises m_schedules, so every
    // call starts from an empty array.
    for (auto i = 0; i < num_schedule; ++i) {
        auto const &entry = SCHEDULES_START[i];
        auto const slot = static_cast<usize>(entry.discipline);
        DEBUG_ASSERT(slot < m_schedules.size(), "a registered discipline is out of range");
        DEBUG_ASSERT(!m_schedules[slot], "two disciplines registered the same policy slot");
        m_schedules[slot] = entry.instance;
    }

    m_run_queue.m_which_cpu = cpunum;
}

auto Scheduler::assert_thread_in_scheduler(Thread *thread) -> void {
    [[maybe_unused]]
    auto &ctx = thread->scheduling_entity();

    DEBUG_ASSERT(ctx.curr_cpu() == m_run_queue.m_which_cpu);
    DEBUG_ASSERT(ctx.schedule() == get_schedule(thread));
}

auto Scheduler::set_state(State state, bool on) -> void {
    // A read-modify-write on a word shared by every CPU, so it has to be a
    // CAS loop and not load/set/store: two CPUs going idle together would
    // otherwise each store a mask missing the other's bit. std::atomic of a
    // BitSet has no fetch_or, hence the loop rather than one call; the same
    // update in Zircon is `mp.idle_cpus.fetch_or(cpu_num_to_mask(cpu))`.
    // Relaxed is enough: the mask is a hint for IPI targeting, and no other
    // data is published through it.
    auto &word = STATES[usize(state)];
    auto expected = word.load(std::memory_order_relaxed);
    for (;;) {
        auto desired = expected;
        desired.set(m_run_queue.m_which_cpu, on);
        if (word.compare_exchange_weak(expected, desired, std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
            return;
        }
    }
}

auto Scheduler::set_schedule(Thread *thread) -> ISchedule * {
    auto &so = thread->scheduling_entity();
    so.m_schedule = get_schedule(thread);
    return so.m_schedule;
}

auto Scheduler::get_schedule(Thread *thread) -> ISchedule * {
    auto &ctx = thread->scheduling_entity();

    auto *const schedule = m_schedules[static_cast<usize>(ctx.discipline())];
    DEBUG_ASSERT(schedule, "No schedule registered for discipline[{}]",
                 static_cast<usize>(ctx.discipline()));

    return schedule;
}

// Takes the mask by reference and copies it, and hands nothing back.
//
// It used to take `CpuMask const *`, swap that pointer into the affinity and
// return the displaced one. Both halves of that had to go with the pointer's
// ownership: storing the caller's address is what let three callers install a
// temporary, and returning the old pointer would now hand back the address of
// a mask living inside the thread, for the caller to hold after the thread has
// moved on. No caller ever read the return value.
//
// Sharing a long-lived mask is deliberately not offered here. The only masks
// that qualify are the kernel's own, and the only code that installs them is
// on_thread_init(), which reaches the affinity directly.
auto Scheduler::set_cpu_affinity(Thread *thread, CpuMask const &affinity, CpuAffinity::Kind kind)
    -> void {
    DEBUG_ASSERT(CpuAffinity::Kind::Soft == kind || CpuAffinity::Kind::Hard == kind);

    auto &so = thread->scheduling_entity();
    switch (kind) {
        using enum CpuAffinity::Kind;
        case Hard:
            so.m_affinity.set_affinity_hard(affinity);
            break;
        case Soft:
            so.m_affinity.set_affinity_soft(affinity);
            break;
    }

    // The thread may be sitting on a CPU its new affinity forbids, in which
    // case it has to be moved before it runs again. Checked after the install,
    // because it is the new affinity that decides.
    if (auto const cpu_thread_on = so.curr_cpu(); cpu_thread_on != INVALID_CPU_NUM) {
        if (so.m_affinity.get_available_mask(CpuMask::of(cpu_thread_on)).none()) {
            Scheduler::migrate_thread(thread);
        }
    }
}

auto Scheduler::tick_thread(Thread *current, SchedTime now) -> void {
    m_canary.verify<Self>();

    // Nested single Guards rather than one ScopedGuard over both. ScopedGuard
    // acquires the same two locks in the same order and releases them in
    // reverse, so this is not a change of order -- it is a change of who can
    // see the order. ScopedGuard is invisible to both the thread-safety
    // analysis and to lockdep, because a capability attribute cannot name a
    // parameter pack (guard.hpp says so on the class itself, and says nested
    // Guards are what callers who need to be seen should write). So this path
    // held the run-queue lock in a way no checker could observe, on the one
    // function every tick goes through.
    //
    // The order is the tree's: a thread's PI lock outside, the run queue's
    // lock inside. Zircon's tick path takes the same pair the same way round.
    //
    // Interrupts are already off -- this runs from the tick -- and both guards
    // are IrqSave all the same: each lock is also taken in thread context, and
    // is only ever held with interrupts off.
    Guard<SpinLock, IrqSave> pi_guard{current->get_pi_lock()};
    Guard<SpinLock, IrqSave> rq_guard{get_lock()};

    m_run_queue.m_clock_source.update_clock();

    auto &ctx = current->scheduling_entity();
    ctx.schedule()->update_thread(m_run_queue, current);
}

auto Scheduler::migrate_thread(Thread *thread) -> void {
    [[maybe_unused]]
    auto &ctx = thread->scheduling_entity();

    if (thread->is_state_ready()) {
        // Dequeue from the scheduler which it is on and Enqueue
        // onto the selected scheudler's runqueue.
    } else if (thread->is_state_running()) {
        // Trigger the IPI for rescheduling
    } else {
    }
}

auto Scheduler::steal_work_from(CpuMask const &mask) -> Thread * {
    return 0;
}

auto Scheduler::process_migrating_threads(SchedTime now) -> void {
    Thread::MigratingList migrating_threads;
    {
        Guard<SpinLock, IrqSave> guard{m_run_queue.m_migrating_threads_lock};
        m_run_queue.m_migrating_threads.swap(migrating_threads);
    }

    CpuMask to_resched;
    while (!migrating_threads.empty()) {
        auto const to_migrate = &migrating_threads.front().value();
        migrating_threads.pop_front();

        // To insert
        auto const target_cpu = Self::find_active_scheduler_for(
            to_migrate, [](Thread *thread, Scheduler *self) { self->finish_transition(thread); });

        to_resched.set(target_cpu);
    }

    reschedule_mask(to_resched);
}

auto Scheduler::finish_transition(Thread *thread) -> void {
    auto &rqs = thread->scheduling_entity().runqueue_state();
    DEBUG_ASSERT(rqs.is_transient_state_migrating());

    using TransientState = RunQueueState::TransientState;
    if (rqs.m_transient_state == TransientState::Migrating) {
    }
}

auto Scheduler::evaluate_next_thread(Thread *curr) -> Thread * {
    auto *const next = pick_next_thread(curr);

    // The hand-over. It used to sit in a gktl::defer above the selection, so
    // that it ran on each of the six `return`s the selection has. Straight-line
    // code now: pick_next_thread() has one job and this has the other, and
    // neither reads a variable the other may not have written yet.
    put_prev_thread(curr);
    set_next_thread(next);
    return next;
}

auto Scheduler::pick_next_thread(Thread *curr) -> Thread * {
    // A thread nominated by a directed yield wins outright. Taken and cleared
    // in one step: the slot carries the nomination across two critical
    // sections (Current::yield_to_thread records it and drops the lock;
    // reschedule_common takes the lock again and reads it), so leaving a stale
    // one behind would hand the same thread over twice.
    if (auto *const nominated = std::exchange(m_run_queue.m_next_thread, nullptr); nominated) {
        return nominated;
    }

    if (auto &ctx = curr->scheduling_entity();
        ctx.discipline() == Discipline::Normal &&
        ctx.schedule()->num_queued(m_run_queue) == m_run_queue.m_num_queued_threads) {
        if (auto *const next = ctx.schedule()->evaluate_next_thread(m_run_queue, curr); next) {
            return next;
        }
    }

    // m_schedules is indexed by Discipline and a slot is null when no
    // discipline of that kind is registered -- Deadline has none today. The
    // loop used to call through every slot regardless; that survived for as
    // long as the first slot always had something to run, and died the first
    // time a CPU ran out of work: a virtual call through a null ISchedule*
    // reads its vtable from address 0, which the boot has left mapped, and
    // jumps into the zero page. No fault, no message, just a CPU gone.
    for (auto *const schedule: m_schedules) {
        if (!schedule) {
            continue;
        }
        if (auto *const next = schedule->evaluate_next_thread(m_run_queue, curr); next) {
            return next;
        }
    }

    if (auto *const next = steal_work_from(Cpu::online_mask()); next) {
        return next;
    }

    if (curr->is_state_running()) {
        // There are no other runnable threads and |curr| is not parked
        // (|curr| from timeslice expired or an yield). so give it a chance
        // to run again.
        return curr;
    }

    // No any thread active, so return the idle thread.
    return m_idle_thread;
}

auto Scheduler::complete_context_switch(Thread *curr) -> void {
    Self &self = Scheduler::Current::get();

    auto const prev_thread = std::exchange(self.m_run_queue.m_prev_thread, nullptr);
    prev_thread->get_pi_lock().release();

    self.set_state(State::Idling, curr == self.m_idle_thread);
}

auto Scheduler::switch_context(Thread *curr, Thread *next) -> void {
    m_run_queue.m_prev_thread = curr;
    m_run_queue.m_curr_thread = next;

    // Make sure that the same aspace doesn't be swaped out, which will
    // lead to flush TLB fully without global pages.
    //
    // Note here we assump that all user address spaces include a duplication
    // of kernel address space.
    if (curr->aspace() != next->aspace()) {
        mm::VmAspace::switch_aspace(curr->aspace(), next->aspace());
    }

    // The incoming thread's PI lock is taken here, by the outgoing side, and
    // travels across the switch: the thread that resumes on the other end
    // finds its own lock held -- the state its caller's guard (or
    // trampoline_handoff(), for a first run) expects to release -- while
    // finish_switch_context() there drops ours. Zircon holds next_thread's
    // lock through the switch the same way and drops the previous thread's
    // in LockHandoffInternal(). Without this, a thread resumed by a switch
    // released a lock nobody held and died on assert_held().
    next->get_pi_lock().acquire();

    // Before to switch context, drop current scheduler's lock
    get_lock().release();
    Thread::switch_thread(curr, next);

    // Except with those newly spawned threads, which entried Thread::trampoline,
    // every threads should go back to here and then run.
    //
    // And there are following things about local variables to note:
    //  1. |next| after context switch is in unknown state, which inable to be used again.
    //  2. |curr| after context switch is the **real** current thread.
    //  3. |curr| before context switch was cached in |m_run_queue.scheduling_entity.prev_thread|.
    //
    // Everything past the switch belongs to whichever CPU is running this
    // thread *now*, which is why complete_context_switch() asks
    // Current::get() rather than using `this`. `this` is the scheduler of the
    // CPU this thread was on when it went into the switch; a thread that
    // sleeps here is placed again by select_target_cpu() when it is woken,
    // and that is free to answer with another CPU, leaving `this` pointing at
    // a scheduler belonging to someone else.
    //
    // Note that this is not only about prev_thread. complete_context_switch()
    // also does set_state(State::Idling, curr == m_idle_thread), and both the
    // state and m_idle_thread are the resuming CPU's. So handing the previous
    // thread across in a register -- the way Linux's switch_to() rewrites its
    // `last` argument -- would remove the field but not this lookup; see
    // docs/changes/260912/task-arch-seam.md for why that trade was declined.
    //
    // trampoline_handoff() -- the other way out of a switch, for a thread that
    // has never run before -- has always asked Current::get(). The two exits
    // from the same switch disagreed; this is the one that was wrong.
    Self::complete_context_switch(curr);
}

auto Scheduler::update_runtime() -> void {
    m_run_queue.m_clock_source.update_clock();
}

auto Scheduler::reschedule_common(Thread *curr, Mode mode) -> void {
    DEBUG_ASSERT(SpinLock::num_held() > 0, "Enter {} with spinlocks held", __func__);
    DEBUG_ASSERT(arch::irqs_disabled(), "Enter {} with IRQ enabled", __func__);
    DEBUG_ASSERT(curr->preemption_state().preemption_disabled_count() != 0,
                 "Enter {}() with preempt enabled", __func__);
    DEBUG_ASSERT(m_run_queue.m_which_cpu == Cpu::id(), "Invoke {}() on mismatched CPU[{}]",
                 __func__, Cpu::id());
    DEBUG_ASSERT(curr == m_run_queue.m_curr_thread);

    assert_thread_in_scheduler(curr);

    if constexpr (SCHED_HPTICK) {
        if (has_feature(sched::Feature::HpTick)) {
        }
    }

    auto const is_mode_preempt = Mode::Preempt == mode;
    auto const is_involuntary = is_mode_preempt || curr->is_state_running();

    auto &so = curr->scheduling_entity();
    auto &rqs = so.runqueue_state();

    rqs.set_transient_state_rescheduling();

    // Grabs this scheduler's runqueue lock.
    //
    // IrqSave over interrupts the entry assertion has already seen off, so it
    // saves and later restores "off". The saver belongs to this frame, not to
    // the lock: unguard() below hands the lock to switch_context(), which
    // drops it before the switch, while the saver stays here until this thread
    // is switched back in and returns -- restoring "off" for the caller, whose
    // own guard then restores what it saved.
    Guard<SpinLock, IrqSave> guard{get_lock()};

    // When unparking a thread, in |Scheduler::enqueue_thread| the clock had been updated
    // and send to the clock driver a skip request. Therefore we firstly try to response the
    // possible request.
    m_run_queue.m_clock_source.accept_skip_next_clock_update();
    m_run_queue.m_clock_source.update_clock();
    m_run_queue.m_clock_source.note_clock_updated();

    curr->preemption_state().flush_pending_remote();

    if (Mode::Idle == mode) {
        if (!num_runnable()) {
            // |guard| release point.
            return;
        }
    } else if (!is_involuntary) {
        // The |curr| is sleeping. Off the runqueue entirely -- not `Migrating`,
        // which promises an imminent landing on another CPU's runqueue and
        // would make `activate_thread` mistake the eventual wakeup for a
        // completed migration (see this file's header comment).
        // TODO(SmallHuaZi): dequeue_thread may also return Status::Delayed, in
        // which case |curr| is still on the runqueue and neither None nor
        // Queueing describes it -- it falls through here and keeps the
        // Rescheduling state set above. RunQueueState::TransientState declares
        // a DequeueDelayed for exactly this, but has no setter or predicate for
        // it yet (see context.hpp).
        using enum DequeueOptions;
        if (Status::Ok == dequeue_thread(curr, SkipClock | Sleep)) {
            rqs.set_transient_state_none();
        }
    }

    auto const next = evaluate_next_thread(curr);
    if (next == curr) [[unlikely]] {
        // |guard| release point.
        return;
    }

    curr->scheduling_entity().metric().inc_context_switch_count(is_involuntary);
    m_num_switches += 1;

    // Note `m_spinlock` (namely lock of runqueue) will be droped in Self::switch_context.
    // For preventing duplicated to drop the lock, we should tell the |guard| that the lock
    // has been released.
    guard.unguard();

    // Everything be ok. Let's proceed the final context switch
    switch_context(curr, next);
}

auto Scheduler::enqueue_thread(Thread *thread, EnqueueOptions options) -> Status {
    m_canary.verify<Self>();

    if (!(options & EnqueueOptions::SkipClock)) {
        m_run_queue.m_clock_source.update_clock();
    }

    auto &so = thread->scheduling_entity();
    auto status = so.schedule()->enqueue_thread(m_run_queue, thread, options);
    if (Status::Ok != status) {
        return status;
    }

    // The thread is on this CPU's queue now, so this is the CPU that owns it.
    // |this| is not necessarily the running CPU -- do_unpark_thread() enqueues
    // into the target's queue from the waker -- which is exactly why the value
    // comes from m_run_queue and not from Cpu::id().
    so.m_curr_cpu = m_run_queue.m_which_cpu;

    if (!!(options & EnqueueOptions::Restore)) {
    }

    m_run_queue.m_num_queued_threads += 1;
    m_run_queue.m_total_weight += so.weight();
    return Status::Ok;
}

auto Scheduler::dequeue_thread(Thread *thread, DequeueOptions options) -> Status {
    m_canary.verify<Self>();

    if (!(options & DequeueOptions::SkipClock)) {
        m_run_queue.m_clock_source.update_clock();
    }

    if (!!(options & DequeueOptions::Save)) {
    }

    auto &so = thread->scheduling_entity();
    auto status = so.schedule()->dequeue_thread(m_run_queue, thread, options);
    if (Status::Ok != status) {
        return status;
    }

    m_run_queue.m_num_queued_threads -= 1;
    m_run_queue.m_total_weight -= so.weight();
    return Status::Ok;
}

auto Scheduler::activate_thread(Thread *thread, EnqueueOptions options) -> void {
    DEBUG_ASSERT(thread);

    m_canary.verify<Self>();

    auto &rqs = thread->scheduling_entity().runqueue_state();
    if (rqs.is_transient_state_migrating()) {
        options |= EnqueueOptions::Migrated;
    }

    // No schedule refuses an enqueue: Fair::enqueue_thread() answers only Ok.
    [[maybe_unused]]
    auto const status = enqueue_thread(thread, options);
    DEBUG_ASSERT(Status::Ok == status);
    rqs.set_transient_state_queueing();
}

auto Scheduler::deactivate_thread(Thread *thread, DequeueOptions options) -> void {
    DEBUG_ASSERT(thread->is_state_sleeping() == false);

    m_canary.verify<Self>();

    auto &rqs = thread->scheduling_entity().runqueue_state();
    rqs.set_transient_state_migrating();

    // Without DequeueOptions::Sleep no schedule delays a dequeue (Fair delays
    // only a sleeper's, try_to_delay_dequeue()), so the thread is off the
    // queue when this returns, as Migrating promises.
    [[maybe_unused]]
    auto const status = dequeue_thread(thread, options);
    DEBUG_ASSERT(Status::Ok == status, "a dequeue for a migration was delayed");
}

auto Scheduler::Current::park_thread(Thread *curr) -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();

    DEBUG_ASSERT(self.m_run_queue.m_which_cpu == Cpu::id());
    DEBUG_ASSERT(curr == self.m_run_queue.m_curr_thread);
    DEBUG_ASSERT(curr->state() & (Thread::State::Parked | Thread::State::Sleeping));

    self.reschedule_common(curr, Mode::Regular);
}

/// `this` is the *target* CPU's scheduler, which is usually not the running
/// one: unpark_thread() selects a CPU for the thread and then reaches into
/// that CPU's run queue from here. The queue is protected by its own lock,
/// taken below, which is what makes the remote insert safe; there used to be a
/// `which_cpu == Cpu::id()` assertion at the top instead, and it held only
/// because select_target_cpu() could never answer anything but CPU 0 -- the
/// first thread placed on a secondary killed the boot on it.
///
/// This is Zircon's Scheduler::Unblock() -> target->Insert(): the insert
/// happens on the waker's CPU, and the target CPU is told about it afterwards
/// through RescheduleMask(), never by running any of this on it.
auto Scheduler::do_unpark_thread(Thread *thread, UnparkOptions options) -> void {
    DEBUG_ASSERT(thread != m_run_queue.m_curr_thread);

    // Every state a thread can be woken out of, not only the two a wait queue
    // produces. Thread::resume() unparks from Initial (a thread that has never
    // run -- this is how start_kernel()'s "laborer" and every other spawned
    // thread first reach a run queue) and from Suspend; the wait queues unpark
    // from Parked and Sleeping. The assertion used to name only the latter
    // pair, so the first resume() of the boot tripped it, and a resume() from
    // Suspend would have too.
    //
    // Zircon's Thread::Resume() routes THREAD_INITIAL and THREAD_SUSPENDED
    // into Scheduler::Unblock() as well, and UnblockCommon() asserts nothing
    // about the source state beyond !IsIdle(): being woken is what moves a
    // thread to READY, whichever non-running state it came from.
    using enum Thread::State;
    DEBUG_ASSERT(thread->state() & (Initial | Suspend | Parked | Sleeping));

    m_canary.verify<Self>();

    auto const enqueue_control = EnqueueOptions::WakeUp;
    auto schedule = set_schedule(thread);

    Guard<SpinLock, IrqSave> guard{get_lock()};

    thread->set_state_ready();
    activate_thread(thread, enqueue_control);

    if constexpr (SCHED_UNPARK_PREEMPT) {
        // Check if |thread| is able to preempt current thread.
        unpark_preempt(thread, options);
    }

    schedule->on_thread_unpark(m_run_queue, thread);
}

/// Asks whether the thread just inserted should preempt what the target CPU is
/// running. Like do_unpark_thread(), which is its only caller, this runs on the
/// waker's CPU and reads the *target* CPU's rq under that CPU's run queue
/// lock.
auto Scheduler::unpark_preempt(Thread *thread, UnparkOptions options) -> void {
    auto const current_thread = m_run_queue.m_curr_thread;
    auto const x_schedule = thread->scheduling_entity().schedule();
    auto const y_schedule = current_thread->scheduling_entity().schedule();

    auto &preemption_state = current_thread->preemption_state();
    if (x_schedule == y_schedule) {
        // To the same schedule, preemption depends on schedule's internal implementation.
        // Fair answers Unimplemented today: a thread woken into the running
        // thread's own schedule never preempts it and waits for its tick
        // (docs/repairs/260917/kernel-task-sched.md, item 2). There is nothing
        // else in the answer to act on.
        auto _ = x_schedule->unpark_preempt(m_run_queue, current_thread, thread, options);
    } else if (x_schedule > y_schedule) {
        m_run_queue.mark_preempt_pending();
    }

    // The queue event has occurred. Therefore there is none of need to process
    // meaningless clock update.
    if (preemption_state.has_preempt_request()) {
        m_run_queue.m_clock_source.request_skip_next_clock_update();
    }
}

auto Scheduler::select_target_cpu(Thread *thread) -> CpuNum {
    DEBUG_ASSERT(thread);

    auto &se = thread->scheduling_entity();

    // Only ask the discipline if the thread has one. A thread reaches here
    // once before it is bound to any -- on_thread_init() calls this on a
    // context whose m_schedule is still null -- and reading through that
    // pointer branched to address 0 and faulted on the instruction fetch.
    // "No discipline yet" and "the discipline has no preference" want the
    // same answer, and the fallback below already is that answer.
    auto target_cpu = INVALID_CPU_NUM;
    if (auto *const schedule = se.schedule()) {
        target_cpu = schedule->select_target_cpu(thread, Cpu::online_mask());
    }
    if (target_cpu != INVALID_CPU_NUM) {
        return target_cpu;
    }

    // The following is a fallback strategy: pick the least loaded scheduler.
    //
    // Load is the run queue's depth, `num_queued_threads`, which enqueue and
    // dequeue keep up to date. It used to be `total_expected_runtime`, a field
    // of RunQueue that nothing in the tree ever writes -- so every comparison
    // was 0 > 0 and the answer was always the lowest-numbered candidate.
    // Nothing showed until the secondaries came up: with one CPU online the
    // degenerate answer is also the right one. With four, every thread the
    // kernel ever created still went to CPU 0 while three CPUs idled.
    //
    // Another CPU's counter is read without its run queue lock. That is
    // deliberate and is what Linux's find_idlest_cpu() does with
    // `cpu_rq(cpu)->nr_running`: the number is a hint that a migration or a
    // re-selection may invalidate a moment later anyway, and taking three
    // remote run queue locks to place one thread would cost more than the
    // occasional misplacement.
    auto const pick_least_loaded = [](CpuMask const &candidates) -> CpuNum {
        auto best_cpu = INVALID_CPU_NUM;
        auto best_load = usize{0};
        // The incumbent's load is carried, not re-read. Reading it again per
        // candidate doubles the remote loads, and each one pulls in a
        // scheduler cacheline that its owning CPU dirties on every enqueue and
        // dequeue -- so the redundant half is pure coherence traffic on the
        // wakeup path.
        candidates.for_each([&best_cpu, &best_load](CpuNum cpunum) {
            auto const load = Scheduler::Current::get(cpunum).num_runnable();
            if (best_cpu == INVALID_CPU_NUM || load < best_load) {
                best_cpu = cpunum;
                best_load = load;
            }
        });
        return best_cpu;
    };

    auto &affinity = se.cpu_affinity();

    // The Active mask is tried first, and not merely because an inactive
    // scheduler is one that has not declared itself ready for threads:
    // find_active_scheduler_for() spins until the CPU it is handed reports
    // Active, so answering with an inactive CPU that never activates would
    // livelock it rather than misplace a thread.
    target_cpu = pick_least_loaded(affinity.get_available_mask(peek_active_mask()));
    if (target_cpu != INVALID_CPU_NUM) {
        return target_cpu;
    }

    // Nothing is Active anywhere. On the boot path that is the ordinary state
    // and not an error: task::init() walks cpu_possible_mask() building every
    // CPU's idle thread, and Scheduler::Current::set_active(true) only runs
    // once that loop has finished, so the whole of early thread creation
    // happens against an empty Active mask. The intersection above came back
    // empty because the mask it intersected with was empty, not because the
    // thread is unrunnable somewhere.
    //
    // So widen to the online mask and prefer the CPU the thread starts from --
    // where it last ran, or, for one that has never run, the CPU creating it.
    // That is the answer `starting_cpu` was computed for before this fallback
    // was left half-written, and leaving a thread where it already is, is what
    // Linux's select_task_rq() does when it has no better idea. Nothing is
    // decided for good here either way: do_unpark_thread() re-selects through
    // find_active_scheduler_for() when the thread actually becomes runnable.
    auto const last_cpu = se.last_cpu();
    auto const starting_cpu = last_cpu == INVALID_CPU_NUM ? Cpu::id() : last_cpu;

    // Widening the candidates must not widen the affinity: `starting_cpu` is a
    // preference, never a licence to ignore a hard pin, so it is only taken
    // when the thread is actually permitted there.
    auto const permitted_mask = affinity.get_available_mask(Cpu::online_mask());
    if (permitted_mask.test(starting_cpu)) {
        return starting_cpu;
    }

    target_cpu = pick_least_loaded(permitted_mask);

    // Now a genuine error rather than a bring-up artifact: the thread's hard
    // affinity excludes every online CPU, so there is nowhere it may run.
    DEBUG_ASSERT(target_cpu != INVALID_CPU_NUM, "At least one CPU was assigned to us");
    return target_cpu;
}

template <typename F, typename... Args>
auto Scheduler::find_active_scheduler_for(Thread *thread, F f, Args &&...args) -> CpuNum {
    while (true) {
        auto const target_cpu = Self::select_target_cpu(thread);
        auto &self = Scheduler::Current::get(target_cpu);

        Guard<SpinLock, IrqSave> guard{self.get_lock()};
        if (self.is_state_active()) {
            f(thread, &self, ustl::forward<Args>(args)...);
            return target_cpu;
        }
    }
}

auto Scheduler::Current::preempt(Thread *curr) -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();
    DEBUG_ASSERT(curr == self.m_run_queue.m_curr_thread);

    return self.reschedule_common(curr, Mode::Preempt);
}

auto Scheduler::Current::reschedule(Thread *curr) -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();
    DEBUG_ASSERT(curr == self.m_run_queue.m_curr_thread);

    // No preemptibility check here any more: the caller's guard has already
    // disabled interrupts and preemption by the time this runs, so the check
    // could only ever fail. Thread::Current::reschedule() makes it first.
    return self.reschedule_common(curr, Mode::Regular);
}

auto Scheduler::Current::idle(Thread *curr) -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();
    DEBUG_ASSERT(curr == self.m_run_queue.m_curr_thread);

    return self.reschedule_common(curr, Mode::Idle);
}

auto Scheduler::Current::yield_thread(Thread *current) -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();

    DEBUG_ASSERT(current == self.m_run_queue.m_curr_thread);

    auto &so = current->scheduling_entity();
    {
        // The runqueue lock has to be dropped before reschedule_common(): that
        // routine takes get_lock() itself and hands it on to switch_context().
        // SpinLock is not reentrant, so holding it across the call spins this
        // CPU on its own lock forever. yield_to_thread() below has always
        // scoped its guard this way; this one did not, and the inverted
        // TA_REQUIRE(get_lock()) on reschedule_common() kept the analysis
        // from noticing.
        Guard<SpinLock, IrqSave> guard{self.get_lock()};

        self.m_run_queue.m_clock_source.update_clock();
        so.schedule()->yield_thread(self.m_run_queue, current);
        self.m_run_queue.m_clock_source.request_skip_next_clock_update();
    }

    self.reschedule_common(current, Mode::Regular);
}

auto Scheduler::Current::yield_to_thread(Thread *curr, Thread *thread) -> Status {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();

    DEBUG_ASSERT(curr == self.m_run_queue.m_curr_thread);

    auto &curr_so = curr->scheduling_entity();
    auto &next_so = thread->scheduling_entity();

    Status yielded;
    {
        Guard<SpinLock, IrqSave> guard{self.get_lock()};

        if (curr_so.schedule() != next_so.schedule()) {
            return Status::BadState;
        }

        yielded = curr_so.schedule()->yield_to_thread(self.m_run_queue, curr, thread);
        if (Status::Ok == yielded) {
            SCHEDULE_YIELD_PAST.add(1);
        }
    }

    if (Status::Ok == yielded) {
        self.reschedule_common(curr, Mode::Regular);
    }

    return yielded;
}

auto Scheduler::Current::tick(SchedTime now) -> void {
    auto const current_thread = Thread::Current::get();
    current_thread->m_canary.verify<Thread>();

    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();
    self.tick_thread(current_thread, now);
}

auto Timeslice::evaluate() -> bool {
    return false;
}

} // namespace ours::task::sched