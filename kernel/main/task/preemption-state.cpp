#include <ours/task/preemption-state.hpp>
#include <ours/task/thread.hpp>

#include <ours/ipi-event.hpp>

namespace ours::task {
    auto PreemptionState::flush_pending_local() -> void {
        auto const this_cpu = Cpu::id();
        if (auto target = m_pendings.load(); target.test(this_cpu)) {
            m_pendings.store(target.set(this_cpu, 0));
            Thread::Current::preempt();
        }
    }

    auto PreemptionState::flush_pending_remote() -> void {
        auto const this_cpu = Cpu::id();
        if (auto target = m_pendings.load(); target.set(this_cpu, 0).any()) {
            mp_reschedule(target, 0);

            clear_pending();
        }
    }

    auto PreemptionState::flush_pending() -> void {
        flush_pending_remote();

        flush_pending_local();
    }

} // namespace ours::task