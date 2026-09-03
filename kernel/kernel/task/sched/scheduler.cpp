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
/// accounting, `m_runtime.num_queued_threads` and `m_runtime.total_weight`.
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
///                        adds EnqueueControl::Migrated, so the discipline can
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
/// EnqueueControl::Migrated. deactivate_thread asserts this distinction with
/// `DEBUG_ASSERT(thread->is_state_sleeping() == false)`.
///
/// TODO(SmallHuaZi): both return void and discard the Status underneath them,
/// so a Status::Delayed dequeue leaves a thread marked Migrating while it is in
/// fact still queued.

#include <arch/auto_interrupt_disabler.hpp>
#include <gktl/counter.hpp>
#include <gktl/defer.hpp>
#include <ktl/new.hpp>
#include <ktl/rwonce.hpp>
#include <ours/cpu.hpp>
#include <ours/ipi_service.hpp>
#include <ours/lockdep.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/sched/context.hpp>
#include <ours/task/sched/features.hpp>
#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/scheduler.hpp>
#include <ours/task/sched/scheduler_inline.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/timer_queue.hpp>

#include <utility>

using ustl::sync::MemoryOrder;

using ours::sync::SpinLock;

namespace ours::task::sched {
namespace {
    extern "C" ScheduleEntry const SCHEDULES_START[] LINK_NAME("__schedules_start");
    extern "C" ScheduleEntry const SCHEDULES_END[] LINK_NAME("__schedules_end");

    GKTL_COUNTER(SCHEDULE_IDLE_PAST, "schedule.idle_past");
    GKTL_COUNTER(SCHEDULE_PREEMPT_PAST, "schedule.preempt_past");
    GKTL_COUNTER(SCHEDULE_YIELD_PAST, "schedule.yield_past");

    auto is_preemptible(Thread *curr) -> bool {
        auto &preemption_state = curr->preemption_state();
        return preemption_state.is_preemption_enabled() && !arch::irqs_disabled();
    }

} // namespace

auto Scheduler::trampoline_handoff() -> void {
    arch::local_irq_enable();

    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();
    self.finish_switch_context();

    self.m_runtime.curr_thread->get_pi_lock().release();
}

auto Scheduler::on_thread_init(Thread *thread, BaseProfile const &profile, CpuNum target_cpu)
    -> void {
    auto &context = thread->scheduling_context();

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
            context.m_affinity.set_affinity_hard_shared(Cpu::online_mask());
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
        schedule->on_thread_init(self.m_runtime, thread);
    }
}

auto Scheduler::on_thread_dead(Thread *thread) -> void {
    [[maybe_unused]]
    auto &ctx = thread->scheduling_context();
    // se.schedule()->on_thread_dead(m_runtime, thread);
}

/// Unpark don't really cause entry into reschedule routine. It just
/// add a task to the run-queue.
///
/// If the new task added to the run-queue preempts the current task,
/// then the Scheduler::unpark_preempt will call PreemptionState::add_pending()
/// to set preemption flags reschedule() gets called on the nearest possible
/// occasion.
auto Scheduler::unpark_thread(Thread *thread, UnparkControl control) -> void {
    DEBUG_ASSERT(thread != Thread::Current::get());

    /// Assign CPU for passed |thread|
    CpuNum target_cpu;
    if (!!(control & UnparkControl::ThisCpu)) {
        target_cpu = Cpu::id();
    } else {
        target_cpu = select_target_cpu(thread);
    }

    auto &self = Scheduler::Current::get(target_cpu);
    self.m_canary.verify<Self>();

    self.do_unpark_thread(thread, control);

    reschedule_mask(CpuMask::of(target_cpu));
}

auto Scheduler::unpark_thread(Thread::UnparkList threads, UnparkControl control) -> void {
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
        Guard thread_guard{thread->get_pi_lock()};

        CpuNum target_cpu;
        if (!!(control & UnparkControl::ThisCpu)) {
            target_cpu = Cpu::id();
        } else {
            target_cpu = select_target_cpu(thread);
        }

        auto &self = Self::Current::get(target_cpu);
        self.m_canary.verify<Self>();

        self.do_unpark_thread(thread, control);

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

    // Local preempt is pending.
    Scheduler::Current::preempt(current_thread);
}

INIT_CODE auto Scheduler::init() -> void {
    new (this) Self();

    auto const num_schedule = SCHEDULES_END - SCHEDULES_START;
    DEBUG_ASSERT(num_schedule > 0);

    for (auto i = 0; i < num_schedule; ++i) {
        auto const &entry = SCHEDULES_START[i];
        m_schedules[static_cast<usize>(entry.discipline)] = entry.instance;
    }

    m_runtime.which_cpu = Cpu::id();
}

auto Scheduler::assert_thread_in_scheduler(Thread *thread) -> void {
    [[maybe_unused]]
    auto &ctx = thread->scheduling_context();

    DEBUG_ASSERT(ctx.curr_cpu() == m_runtime.which_cpu);
    DEBUG_ASSERT(ctx.schedule() == get_schedule(thread));
}

auto Scheduler::set_state(State state, bool on) -> void {
    auto active_mask = STATES[usize(state)].load(MemoryOrder::Relaxed);
    active_mask.set(m_runtime.which_cpu, on);
    STATES[usize(state)].store(active_mask, MemoryOrder::Relaxed);
}

auto Scheduler::set_schedule(Thread *thread) -> ISchedule * {
    auto &so = thread->scheduling_context();
    so.m_schedule = get_schedule(thread);
    return so.m_schedule;
}

auto Scheduler::get_schedule(Thread *thread) -> ISchedule * {
    auto &ctx = thread->scheduling_context();

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

    auto &so = thread->scheduling_context();
    switch (kind) {
        case CpuAffinity::Kind::Hard:
            so.m_affinity.set_affinity_hard(affinity);
            break;
        case CpuAffinity::Kind::Soft:
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

    ScopedGuard lock_guard{current->get_pi_lock(), get_lock()};

    m_runtime.driver.update_clock();

    auto &ctx = current->scheduling_context();
    ctx.schedule()->update_thread(m_runtime, current);
}

auto Scheduler::migrate_thread(Thread *thread) -> void {
    [[maybe_unused]]
    auto &ctx = thread->scheduling_context();

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
        Guard guard{m_runtime.migrating_threads_lock};
        m_runtime.migrating_threads.swap(migrating_threads);
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
    auto &rqs = thread->scheduling_context().runqueue_state();
    DEBUG_ASSERT(rqs.is_transient_state_migrating());

    using TransientState = RunQueueState::TransientState;
    if (rqs.m_transient_state == TransientState::Migrating) {
    }
}

auto Scheduler::evaluate_next_thread(Thread *curr) -> Thread * {
    Thread *next;
    auto const deferred = gktl::defer([this, curr, &next] FORCE_INLINE_ATTR() {
        put_prev_thread(curr);
        set_next_thread(next);
    });

    if (next = m_runtime.next_thread; next != nullptr) {
        m_runtime.next_thread = nullptr;
        return next;
    }

    if (auto &ctx = curr->scheduling_context();
        ctx.discipline() == Discipline::Normal &&
        ctx.schedule()->num_queued() == m_runtime.num_queued_threads) {
        if (next = ctx.schedule()->evaluate_next_thread(m_runtime, curr); next) {
            return next;
        }
    }

    for (auto &schedule: m_schedules) {
        if (next = schedule->evaluate_next_thread(m_runtime, curr); next) {
            return next;
        }
    }

    if (next = steal_work_from(Cpu::online_mask()); next) {
        return next;
    }

    if (curr->is_state_running()) {
        // There are no other runnable threads and |curr| is not parked
        // (|curr| from timeslice expired or an yield). so give it a chance
        // to run again.
        return curr;
    }

    // No any thread active, so return the idle thread.
    next = m_idle_thread;
    return next;
}

auto Scheduler::finish_switch_context() -> void {
    auto const prev_thread = std::exchange(m_runtime.prev_thread, nullptr);
    prev_thread->get_pi_lock().release();

    set_state(State::Idling, m_runtime.curr_thread == m_idle_thread);
}

auto Scheduler::switch_context(Thread *curr, Thread *next) -> void {
    m_runtime.prev_thread = curr;
    m_runtime.curr_thread = next;

    // Make sure that the same aspace doesn't be swaped out, which will
    // lead to flush TLB fully without global pages.
    //
    // Note here we assump that all user address spaces include a duplication
    // of kernel address space.
    if (curr->aspace() != next->aspace()) {
        mm::VmAspace::switch_aspace(curr->aspace(), next->aspace());
    }

    // Before to switch context, drop current scheduler's lock
    get_lock().release();
    Thread::switch_thread(curr, next);

    // Except with those newly spawned threads, which entried Thread::trampoline,
    // every threads should go back to here and then run.
    //
    // And there are following things about local variables to note:
    //  1. |next| after context switch is in unknown state, which inable to be used again.
    //  2. |curr| after context switch is the **real** current thread.
    //  3. |curr| before context switch was cached in |m_runtime.scheduling_context.prev_thread|.
    finish_switch_context();
}

auto Scheduler::update_runtime() -> void {
    m_runtime.driver.update_clock();
}

auto Scheduler::reschedule_common(Thread *curr, Mode mode) -> void {
    DEBUG_ASSERT(SpinLock::num_held() > 0, "Enter {} with spinlocks held", __func__);
    DEBUG_ASSERT(arch::irqs_disabled(), "Enter {} with IRQ enabled", __func__);
    DEBUG_ASSERT(curr->preemption_state().preemption_disabled_count() != 0,
                 "Enter {}() with preempt enabled", __func__);
    DEBUG_ASSERT(m_runtime.which_cpu == Cpu::id(), "Invoke {}() on mismatched CPU[{}]", __func__,
                 Cpu::id());
    DEBUG_ASSERT(curr == m_runtime.curr_thread);

    assert_thread_in_scheduler(curr);

    if constexpr (SCHED_HPTICK) {
        if (has_feature(sched::Feature::HpTick)) {
        }
    }

    auto const is_mode_preempt = Mode::Preempt == mode;
    auto const is_involuntary = is_mode_preempt || curr->is_state_running();

    auto &so = curr->scheduling_context();
    auto &rqs = so.runqueue_state();

    rqs.set_transient_state_rescheduling();

    // Grabs this scheduler's runqueue lock.
    Guard guard{get_lock()};

    // When unparking a thread, in |Scheduler::enqueue_thread| the clock had been updated
    // and send to the clock driver a skip request. Therefore we firstly try to response the
    // possible request.
    m_runtime.driver.accept_skip_next_clock_update();
    m_runtime.driver.update_clock();
    m_runtime.driver.note_clock_updated();

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
        using enum DequeueControl;
        if (Status::Ok == dequeue_thread(curr, SkipClock | Sleep)) {
            rqs.set_transient_state_none();
        }
    }

    auto const next = evaluate_next_thread(curr);
    if (next == curr) [[unlikely]] {
        // |guard| release point.
        return;
    }

    curr->scheduling_context().metric().inc_context_switch_count(is_involuntary);
    m_num_switches += 1;

    // Note `m_spinlock` (namely lock of runqueue) will be droped in Self::switch_context.
    // For preventing duplicated to drop the lock, we should tell the |guard| that the lock
    // has been released.
    guard.unguard();

    // Everything be ok. Let's proceed the final context switch
    switch_context(curr, next);
}

auto Scheduler::enqueue_thread(Thread *thread, EnqueueControl control) -> Status {
    m_canary.verify<Self>();

    if (!(control & EnqueueControl::SkipClock)) {
        m_runtime.driver.update_clock();
    }

    auto &so = thread->scheduling_context();
    auto status = so.schedule()->enqueue_thread(m_runtime, thread, control);
    if (Status::Ok != status) {
        return status;
    }

    if (!!(control & EnqueueControl::Restore)) {
    }

    m_runtime.num_queued_threads += 1;
    m_runtime.total_weight += so.weight();
    return Status::Ok;
}

auto Scheduler::dequeue_thread(Thread *thread, DequeueControl control) -> Status {
    m_canary.verify<Self>();

    if (!(control & DequeueControl::SkipClock)) {
        m_runtime.driver.update_clock();
    }

    if (!!(control & DequeueControl::Save)) {
    }

    auto &so = thread->scheduling_context();
    auto status = so.schedule()->dequeue_thread(m_runtime, thread, control);
    if (Status::Ok != status) {
        return status;
    }

    m_runtime.num_queued_threads -= 1;
    m_runtime.total_weight -= so.weight();
    return Status::Ok;
}

auto Scheduler::activate_thread(Thread *thread, EnqueueControl control) -> void {
    DEBUG_ASSERT(thread);

    m_canary.verify<Self>();

    auto &rqs = thread->scheduling_context().runqueue_state();
    if (rqs.is_transient_state_migrating()) {
        control |= EnqueueControl::Migrated;
    }

    enqueue_thread(thread, control);
    rqs.set_transient_state_queueing();
}

auto Scheduler::deactivate_thread(Thread *thread, DequeueControl control) -> void {
    DEBUG_ASSERT(thread->is_state_sleeping() == false);

    m_canary.verify<Self>();

    auto &rqs = thread->scheduling_context().runqueue_state();
    rqs.set_transient_state_migrating();

    dequeue_thread(thread, control);
}

auto Scheduler::Current::park_thread(Thread *curr) -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();

    DEBUG_ASSERT(self.m_runtime.which_cpu == Cpu::id());
    DEBUG_ASSERT(curr == self.m_runtime.curr_thread);
    DEBUG_ASSERT(curr->state() & (Thread::State::Parked | Thread::State::Sleeping));

    self.reschedule_common(curr, Mode::Regular);
}

auto Scheduler::do_unpark_thread(Thread *thread, UnparkControl control) -> void {
    DEBUG_ASSERT(m_runtime.which_cpu == Cpu::id());
    DEBUG_ASSERT(thread != m_runtime.curr_thread);

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

    auto const enqueue_control = EnqueueControl::WakeUp;
    auto schedule = set_schedule(thread);

    Guard guard{get_lock()};

    thread->set_state_ready();
    activate_thread(thread, enqueue_control);

    if constexpr (SCHED_UNPARK_PREEMPT) {
        // Check if |thread| is able to preempt current thread.
        unpark_preempt(thread, control);
    }

    schedule->on_thread_unpark(m_runtime, thread);
}

auto Scheduler::unpark_preempt(Thread *thread, UnparkControl control) -> void {
    DEBUG_ASSERT(Cpu::id() == m_runtime.which_cpu);

    auto const current_thread = m_runtime.curr_thread;
    auto const x_schedule = thread->scheduling_context().schedule();
    auto const y_schedule = current_thread->scheduling_context().schedule();

    auto &preemption_state = current_thread->preemption_state();
    if (x_schedule == y_schedule) {
        // To the same schedule, preemption depends on schedule's internal implementation.
        x_schedule->unpark_preempt(m_runtime, current_thread, thread, control);
    } else if (x_schedule > y_schedule) {
        preemption_state.add_pending();
    }

    // The queue event has occurred. Therefore there is none of need to process
    // meaningless clock update.
    if (preemption_state.has_preempt_request()) {
        m_runtime.driver.request_skip_next_clock_update();
    }
}

auto Scheduler::select_target_cpu(Thread *thread) -> CpuNum {
    DEBUG_ASSERT(thread);

    auto &se = thread->scheduling_context();

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

    // The following is a fallback strategy: pick the scheduler with the minimal runtime.
    auto const pick_least_loaded = [](CpuMask const &candidates) -> CpuNum {
        auto best_cpu = INVALID_CPU_NUM;
        candidates.for_each([&best_cpu](CpuNum cpunum) {
            // Find the mininal expected runtime CPU
            if (best_cpu == INVALID_CPU_NUM ||
                Scheduler::Current::get(best_cpu).m_runtime.total_expected_runtime >
                    Scheduler::Current::get(cpunum).m_runtime.total_expected_runtime) {
                best_cpu = cpunum;
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

        Guard guard{self.get_lock()};
        if (self.is_state_active()) {
            f(thread, &self, ustl::forward<Args>(args)...);
            return target_cpu;
        }
    }
}

auto Scheduler::Current::preempt(Thread *curr) -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();
    DEBUG_ASSERT(curr == self.m_runtime.curr_thread);

    return self.reschedule_common(curr, Mode::Preempt);
}

auto Scheduler::Current::reschedule(Thread *curr) -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();
    DEBUG_ASSERT(curr == self.m_runtime.curr_thread);

    if (!is_preemptible(curr)) {
        curr->preemption_state().add_pending();
        return;
    }

    return self.reschedule_common(curr, Mode::Regular);
}

auto Scheduler::Current::idle(Thread *curr) -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();
    DEBUG_ASSERT(curr == self.m_runtime.curr_thread);

    return self.reschedule_common(curr, Mode::Idle);
}

auto Scheduler::Current::yield_thread(Thread *current) -> void {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();

    DEBUG_ASSERT(current == self.m_runtime.curr_thread);

    auto &so = current->scheduling_context();
    {
        // The runqueue lock has to be dropped before reschedule_common(): that
        // routine takes get_lock() itself and hands it on to switch_context().
        // SpinLock is not reentrant, so holding it across the call spins this
        // CPU on its own lock forever. yield_to_thread() below has always
        // scoped its guard this way; this one did not, and the inverted
        // TA_REQUIRE(get_lock()) on reschedule_common() kept the analysis
        // from noticing.
        Guard guard{self.get_lock()};

        self.m_runtime.driver.update_clock();
        so.schedule()->yield_thread(self.m_runtime, current);
        self.m_runtime.driver.request_skip_next_clock_update();
    }

    self.reschedule_common(current, Mode::Regular);
}

auto Scheduler::Current::yield_to_thread(Thread *curr, Thread *thread) -> Status {
    auto &self = Scheduler::Current::get();
    self.m_canary.verify<Self>();

    DEBUG_ASSERT(curr == self.m_runtime.curr_thread);

    auto &curr_so = curr->scheduling_context();
    auto &next_so = thread->scheduling_context();

    Status yielded;
    {
        ScopedGuard guard{self.get_lock()};

        if (curr_so.schedule() != next_so.schedule()) {
            return Status::BadState;
        }

        yielded = curr_so.schedule()->yield_to_thread(self.m_runtime, curr, thread);
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