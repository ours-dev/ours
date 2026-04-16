#include <ours/task/sched/scheduler.hpp>

#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/context.hpp>
#include <ours/task/sched/features.hpp>

#include <ours/task/thread.hpp>
#include <ours/task/auto-preempt-disabler.hpp>

#include <ours/time/clock.hpp>
#include <ours/time/timer-queue.hpp>

#include <ours/cpu.hpp>
#include <ours/ipi-service.hpp>
#include <ours/lockdep.hpp>

#include <arch/auto-interrupt-disabler.hpp>

#include <ktl/rwonce.hpp>

#include <gktl/counter.hpp>
#include <gktl/defer.hpp>

#include <ustl/util/swap.hpp>

using ustl::sync::MemoryOrder;
using ustl::traits::ConditionalT;

using ours::sync::SpinLock;
using ours::mem::CpuLocal;

namespace ours::task::sched {
namespace {
    extern "C" Schedule *const kSchedulesStart[] LINK_NAME("__schedules_start");
    extern "C" Schedule *const kSchedulesEnd[] LINK_NAME("__schedules_end");

    GKTL_COUNTER(schedule_idle_past, "schedule.idle_past");
    GKTL_COUNTER(schedule_preempt_past, "schedule.preempt_past");
    GKTL_COUNTER(schedule_yield_past, "schedule.yield_past");

    auto is_preemptible(Thread *curr) -> bool {
        auto &preemption_state = curr->preemption_state();
        return preemption_state.is_preemption_enabled() && !arch::irqs_disabled();
    }

    auto is_special_state(Thread *curr) -> bool {
        CXX11_CONSTEXPR
        auto const kThreadStateSpecial = 
            Thread::State::Dead | Thread::State::Sleeping;

        auto const state = curr->state();
        return !!(state & kThreadStateSpecial);
    }

    auto get_schedule(CpuNum cpu, Context &ctx) -> void {
    }

    auto set_schedule(CpuNum cpu, Context &ctx) -> void {
    }
} // namespace anonymous

    auto Scheduler::trampoline_handoff() -> void {
        arch::local_irq_enable();

        auto &self = Scheduler::Current::get();
        self.m_canary.verify<Self>();
        self.finish_switch_context();

        self.m_runtime.curr_thread->get_pi_lock().release();
    }

    auto Scheduler::on_thread_init(Thread *thread, BaseProfile const &profile) -> void {
        auto const target_cpu = select_target_cpu(thread);
        auto &self = Scheduler::Current::get(target_cpu);
        self.m_canary.verify<Self>();

        {
            auto &context = thread->scheduling_context();
            Guard<SpinLock, IrqSave> thread_guard{thread->get_pi_lock()};

            context.schedule()->on_thread_init(self.m_runtime, thread);
            context.m_affinity.set_affinity_hard(Cpu::online_mask());
        }
    }

    auto Scheduler::on_thread_dead(Thread *thread) -> void {
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
            auto *const thread = &threads.front();
            threads.pop_front();

            thread->m_canary.verify<Self>();
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
        current_thread->m_canary.verify<Self>();

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

    INIT_CODE
    auto Scheduler::init() -> void {
        new (this) Self();

        auto const num_schedule = kSchedulesEnd - kSchedulesStart;
        DEBUG_ASSERT(num_schedule > 0);

        auto raw_schedules = new (mem::kGafKernel) Schedule *[num_schedule]();
        DEBUG_ASSERT(raw_schedules, "Failed to allocate memory to place schedules");

        m_schedules = ustl::views::make_span(raw_schedules, num_schedule);
        for (auto i = 0; i < num_schedule; ++i) {
            m_schedules[i] = CpuLocal::access(kSchedulesStart[i]);
        }

        m_runtime.which_cpu = Cpu::id();
    }

    auto Scheduler::assert_thread_in_scheduler(Thread *thread) -> void {
        auto &ctx = thread->scheduling_context();

        DEBUG_ASSERT(ctx.curr_cpu() == m_runtime.which_cpu);
        DEBUG_ASSERT(ctx.schedule() == get_schedule(thread));
    }

    auto Scheduler::set_state(State state, bool on) -> void {
        auto active_mask = g_states[usize(state)].load(MemoryOrder::Relaxed);
        active_mask.set(m_runtime.which_cpu, on);
        g_states[usize(state)].store(active_mask, MemoryOrder::Relaxed);
    }

    auto Scheduler::set_schedule(Thread *thread) -> Schedule * {
        auto &so = thread->scheduling_context();
        so.m_schedule = get_schedule(thread);
        return so.m_schedule;
    }

    auto Scheduler::get_schedule(Thread *thread) -> Schedule * {
        auto &ctx = thread->scheduling_context();

        Schedule *schedule = 0;
        switch (ctx.discipline()) {
            case Discipline::Deadline: 
                schedule = CpuLocal::access(g_fair, m_runtime.which_cpu);
                break;
            default: 
                schedule = CpuLocal::access(g_fair, m_runtime.which_cpu);
                break;
        }

        return schedule;
    }

    auto Scheduler::set_cpu_affinity(Thread *thread, CpuMask const *affinity, CpuAffinity::Kind kind) -> CpuMask const * {
        DEBUG_ASSERT(CpuAffinity::Kind::Soft == kind || CpuAffinity::Kind::Hard == kind);

        auto do_set_affinity = [=, &affinity] FORCE_INLINE_ATTR () mutable {
            auto &cpu_affinity = thread->scheduling_context().cpu_affinity();
            switch (kind) {
                case CpuAffinity::Kind::Hard:
                    ustl::swap(cpu_affinity.m_hard_affinity, affinity);
                    break;
                case CpuAffinity::Kind::Soft:
                    ustl::swap(cpu_affinity.m_soft_affinity, affinity);
                    break;
            }
        };

        auto &so = thread->scheduling_context();

        // Details...

        if (auto const cpu_thread_on = so.curr_cpu(); cpu_thread_on != kInvalidCpuNum) {
            if (so.m_affinity.get_available_mask(CpuMask::of(cpu_thread_on)).none()) {
                Scheduler::migrate_thread(thread);
            }
        }

        return affinity;
    }

    auto Scheduler::tick_thread(Thread *current, SchedTime now) -> void {
        m_canary.verify<Self>();

        ScopedGuard lock_guard{current->get_pi_lock(), get_lock()};

        m_runtime.driver.update_clock();

        auto &ctx = current->scheduling_context();
        ctx.schedule()->update_thread(m_runtime, current);
    }

    auto Scheduler::migrate_thread(Thread *thread) -> void {
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
            auto const to_migrate = &migrating_threads.front();
            migrating_threads.pop_front();

            // To insert
            auto const target_cpu = Self::find_active_scheduler_for(to_migrate, [] (Thread *thread, Scheduler *self) {
                self->finish_transition(thread);
            });

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
        auto const deferred = gktl::defer([this, curr, &next] FORCE_INLINE_ATTR () {
            put_prev_thread(curr);
            set_next_thread(next);
        });

        if (next = m_runtime.next_thread; next != nullptr) {
            m_runtime.next_thread = nullptr;
            return next;
        }

        if (auto &ctx = curr->scheduling_context();
            ctx.discipline() == Discipline::Normal && 
            ctx.schedule()->num_queued() == m_runtime.num_queued_threads)
        {
            if (next = ctx.schedule()->evaluate_next_thread(m_runtime, curr); next) {
                return next;
            }
        }

        for (auto &schedule : m_schedules) {
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
            mem::VmAspace::switch_aspace(curr->aspace(), next->aspace());
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
        DEBUG_ASSERT(m_runtime.which_cpu == Cpu::id(), 
            "Invoke {}() on mismatched CPU[{}]", __func__, Cpu::id());
        DEBUG_ASSERT(curr == m_runtime.curr_thread);

        assert_thread_in_scheduler(curr);

        if CXX20_CONSTEXPR (SCHED_HPTICK) {
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
            // The |curr| is sleeping.
            auto const control = DequeueControl::SkipClock | DequeueControl::Sleep;
            if (Status::Ok == dequeue_thread(curr, control)) {
                rqs.set_transient_state_migrating();
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
        DEBUG_ASSERT(curr ->state() & (Thread::State::Parked | Thread::State::Sleeping));

        self.schedule_common(curr, Mode::Regular);
    }

    auto Scheduler::do_unpark_thread(Thread *thread, UnparkControl control) -> void {
        DEBUG_ASSERT(m_runtime.which_cpu == Cpu::id());
        DEBUG_ASSERT(thread != m_runtime.curr_thread);
        DEBUG_ASSERT(thread->state() & (Thread::State::Parked | Thread::State::Sleeping));

        m_canary.verify<Self>();

        auto const enqueue_control = EnqueueControl::WakeUp;
        auto schedule = set_schedule(thread);

        Guard guard{get_lock()};

        thread->set_state_ready();
        activate_thread(thread, enqueue_control);

        if CXX20_CONSTEXPR (SCHED_UNPARK_PREEMPT) {
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
        auto target_cpu = se.schedule()->select_target_cpu(thread, Cpu::online_mask());
        if (target_cpu != kInvalidCpuNum) {
            return target_cpu;
        }

        // The following is a fallback strategy: pick the scheduler with the minimal runtime.
        auto last_cpu = se.last_cpu();
        auto curr_cpu = Cpu::id();
        auto starting_cpu = last_cpu == kInvalidCpuNum ? curr_cpu : last_cpu;

        auto const active_mask = peek_active_mask();
        auto const available_mask = se.cpu_affinity().get_available_mask(active_mask);
        available_mask.for_each([&target_cpu] (CpuNum cpunum) {
            // Find the mininal expected runtime CPU
            if (target_cpu == kInvalidCpuNum ||
                Scheduler::Current::get(target_cpu).m_runtime.total_expected_runtime >
                Scheduler::Current::get(cpunum).m_runtime.total_expected_runtime) {
                target_cpu = cpunum;
            }
        });

        DEBUG_ASSERT(target_cpu != kInvalidCpuNum, "At least one CPU was assigned to us");
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

        return self.schedule_common(curr, Mode::Preempt);
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
        Guard guard{self.get_lock()};

        self.m_runtime.driver.update_clock();
        so.schedule()->yield_thread(self.m_runtime, current);
        self.m_runtime.driver.request_skip_next_clock_update();

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
                return Status::Fail;
            }

            yielded = curr_so.schedule()->yield_to_thread(self.m_runtime, curr, thread);
            if (Status::Ok == yielded) {
                schedule_yield_past.add(1);
            }
        }

        if (Status::Ok == yielded) {
            self.reschedule_common(curr, Mode::Regular);
        }

        return yielded;
    }

    auto Scheduler::Current::tick(SchedTime now) -> void {
        auto const current_thread = Thread::Current::get();
        current_thread->m_canary.verify<Self>();

        auto &self = Scheduler::Current::get(); 
        self.m_canary.verify<Self>();
        self.tick_thread(current_thread, now);
    }

    auto Timeslice::evaluate() -> bool {
        return false;
    }

} // namespace ours::task::sched