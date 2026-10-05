#include <ours/ipi_event.hpp>
#include <ours/ipi_service.hpp>
#include <ours/task/preemption_state.hpp>
#include <ours/task/thread.hpp>

#include <atomic>

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

    m_timeslice_extension.store(duration, std::memory_order_relaxed);
    m_counter.set<Tag::TimesliceExtension>(true);
    return true;
}

auto PreemptionState::cancel_timeslice_extension() -> void {
    m_counter.set<Tag::TimesliceExtension>(false);
    m_timeslice_extension.store(Duration::zero(), std::memory_order_relaxed);
}

auto PreemptionState::flush_pending_local() -> void {
    if (take_pending_local()) {
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
    m_pendings.store(pending & CpuMask::of(this_cpu));
}

auto PreemptionState::flush_pending() -> void {
    flush_pending_remote();

    flush_pending_local();
}

} // namespace ours::task