#include <ours/irq/irq_dispatcher.hpp>
#include <ours/irq/irq_observer.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/task/thread.hpp>

#include <heap/new.hpp>

namespace ours::irq {
namespace {
    // Trap frame of the interrupt currently being handled on this CPU, or
    // nullptr outside interrupt context. Instrumentation (e.g. the sampling
    // profiler's tick hook) reads it to reach the interrupted context.
    CPU_LOCAL
    arch::IrqFrame *CURRENT_FRAME;
} // namespace

auto current_frame() -> arch::IrqFrame * {
    return mm::CpuLocal::read(CURRENT_FRAME);
}
FORCE_INLINE
static auto get_irq_dispatcher() -> IrqDispatcher * {
    static IrqDispatcher IRQ_DISPATCHER;
    return &IRQ_DISPATCHER;
}

auto init_lines(ustl::views::Span<HIrqNum const> const hirqnums) -> Status {
    return get_irq_dispatcher()->init(hirqnums);
}

auto attach(VIrqNum virqnum, IrqObserver &observer) -> Status {
    return get_irq_dispatcher()->attach(virqnum, observer);
}

auto detach(IrqObserver &observer) -> Status {
    return get_irq_dispatcher()->detach(observer);
}

auto set_affinity(VIrqNum, const CpuMask &targets) -> Status {
    return Status::Ok;
}

auto handle_irq_generic(VIrqNum virqnum) -> IrqReturn {
    ASSERT(virqnum != INVALID_V_IRQ_NUM);
    return get_irq_dispatcher()->handle_irq(virqnum);
}

auto spurious_count(VIrqNum virqnum) -> ustl::Option<u64> {
    return get_irq_dispatcher()->spurious_count(virqnum);
}

/// The bookkeeping on entry to an interrupt: record the frame, and raise the
/// counts that keep this CPU from switching threads until the exit. Runs for
/// every vector that takes the bookkeeping -- device interrupts, IPIs and
/// exceptions such as #PF alike; arch_handle_exception() keeps NMI, #DF and
/// #MC out.
///
/// Raising the two disable counts is what makes the handler non-preemptible.
/// There is deliberately no hard-IRQ count beside them, and PreemptionState's
/// Tag says why. CURRENT_FRAME is this CPU's "handling an interrupt" signal
/// instead.
auto handle_irq_preamble(HIrqNum irqnum, arch::IrqFrame *frame) -> void {
    mm::CpuLocal::write(CURRENT_FRAME, frame);

    auto &preemption_state = task::Thread::Current::preemption_state();
    preemption_state.disable_eager_reschedule();
    preemption_state.disable_preemption();
}

/// The bookkeeping on exit from an interrupt. The interrupt is over; what is
/// decided here is whether the CPU should switch threads before returning to
/// the interrupted context, and the deciding is all this does -- the switch
/// is the caller's (arch_handle_exception), after the interrupt frame is put
/// away.
///
/// Two things this used to get wrong. It called enable_preemption(), which
/// *flushes*: a pending local preemption was performed right here, inside
/// the interrupt glue, and then reported back so the caller preempted a
/// second time. And it reported is_preemption_enabled() alone, so with
/// preemption enabled every interrupt exit asked for a reschedule whether or
/// not anything was pending -- a full pass through reschedule_common() per
/// tick. Neither showed while is_preemption_enabled() was always false.
///
/// The order matters: eager reschedules come back first, which flushes only
/// the remote requests because the preempt count is still up; then the
/// preempt count drops without a flush (enable_preemption_delay_resched),
/// and the local request, if any, is taken and handed to the caller.
auto handle_irq_postamble() -> bool {
    mm::CpuLocal::write(CURRENT_FRAME, static_cast<arch::IrqFrame *>(nullptr));

    auto &preemption_state = task::Thread::Current::preemption_state();
    preemption_state.enable_eager_reschedule();
    preemption_state.enable_preemption_delay_resched();

    return preemption_state.is_preemption_enabled() && preemption_state.take_pending_local();
}

} // namespace ours::irq