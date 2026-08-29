#include <ours/task/preemption_state.hpp>
#include <ours/task/thread.hpp>

#include <ours/ipi_event.hpp>
#include <ours/ipi_service.hpp>

namespace ours::task {
    /// Asks the scheduler to let this thread run a little past the end of its
    /// slice. Mutex::acquire_common() sets one before spinning so that a
    /// thread about to take a lock is not preempted while holding it, and
    /// cancels it the moment the spin fails.
    ///
    /// Returns whether an extension is now in effect.
    auto PreemptionState::set_timeslice_extension(Duration duration) -> bool {
        if (duration <= Duration::zero()) {
            cancel_timeslice_extension();
            return false;
        }

        m_timeslice_extension.store(duration, ustl::sync::MemoryOrder::Relaxed);
        m_counter.set<Tag::TimesliceExtension>(true);
        return true;
    }

    auto PreemptionState::cancel_timeslice_extension() -> void {
        m_counter.set<Tag::TimesliceExtension>(false);
        m_timeslice_extension.store(Duration::zero(), ustl::sync::MemoryOrder::Relaxed);
    }

    auto PreemptionState::flush_pending_local() -> void {
        auto const this_cpu = Cpu::id();
        if (auto target = m_pendings.load(); target.test(this_cpu)) {
            m_pendings.store(target.set(this_cpu, 0));
            Thread::Current::preempt();
        }
    }

    auto PreemptionState::flush_pending_remote() -> void {
        auto const this_cpu = Cpu::id();
        auto const pending = m_pendings.load();

        auto remote = pending;
        remote.set(this_cpu, 0);
        if (!remote.any()) {
            return;
        }

        IpiService::send_resched(remote);

        // Keep the local bit, drop only the remote ones.
        //
        // Only the remote requests have just been paid; a pending local
        // reschedule is still owed a context switch, and flush_pending_local()
        // is what pays it. This used to call clear_pending(), which wiped the
        // whole mask along with the NoResched flags -- so whenever eager
        // rescheduling was re-enabled while preemption was still disabled (the
        // local flush being skipped), the local request was dropped outright
        // and the thread simply never got preempted.
        //
        // Zircon does the same thing for the same reason, in
        // PreemptionState::FlushPendingContinued: on a remote-only flush it
        // ends with `preempts_pending_ &= current_cpu_mask`.
        m_pendings.store(pending & CpuMask::of(this_cpu));
    }

    auto PreemptionState::flush_pending() -> void {
        flush_pending_remote();

        flush_pending_local();
    }

} // namespace ours::task