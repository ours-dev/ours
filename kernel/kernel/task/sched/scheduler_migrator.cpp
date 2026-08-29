#include <ours/task/sched/schedule.hpp>
#include <ours/task/sched/scheduler.hpp>

namespace ours::task::sched {
class Scheduler::Migrator {
  public:
    static auto move_queued_thread(Scheduler *self, Thread *thread, CpuNum newcpu) -> void;
};

auto Scheduler::Migrator::move_queued_thread(Scheduler *self, Thread *thread, CpuNum newcpu)
    -> void {
    self->m_canary.verify<Self>();

    self->deactivate_thread(thread, DequeueControl::SkipClock);

    self->m_runtime.rqlock.release();

    self = &Current::get(newcpu);

    self->m_runtime.rqlock.acquire();

    self->activate_thread(thread, EnqueueControl::None);
    self->unpark_preempt(thread, UnparkControl::None);
}

} // namespace ours::task::sched