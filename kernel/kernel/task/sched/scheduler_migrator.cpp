#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/scheduler.hpp>

namespace ours::task::sched {
class Scheduler::Migrator {
  public:
    /// Enters holding |self|'s runqueue lock and returns holding the *target*
    /// CPU's runqueue lock instead -- the caller's lock is dropped in the
    /// middle and a different scheduler's taken. The analysis can state
    /// "requires L" and "acquires L" but not "swaps L for M", so the body is
    /// opted out; the entry condition is the half that can be checked. The
    /// bare acquire()/release() are deliberate here for the same reason a
    /// Guard cannot express the swap.
    TA_REQUIRES(self->get_lock())
    TA_NO_THREAD_SAFETY_ANALYSIS
    static auto move_queued_thread(Scheduler *self, Thread *thread, CpuNum newcpu) -> void;
};

auto Scheduler::Migrator::move_queued_thread(Scheduler *self, Thread *thread, CpuNum newcpu)
    -> void {
    self->m_canary.verify<Self>();

    self->deactivate_thread(thread, DequeueOptions::SkipClock);

    self->m_run_queue.m_rqlock.release();

    self = &Current::get(newcpu);

    self->m_run_queue.m_rqlock.acquire();

    self->activate_thread(thread, EnqueueOptions::None);
    self->unpark_preempt(thread, UnparkOptions::None);
}

} // namespace ours::task::sched