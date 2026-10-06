#include <ours/irq/action.hpp>
#include <ours/irq/desc_table.hpp>
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
static auto get_desc_table() -> DescTable * {
    static DescTable table;
    return &table;
}

auto init_lines(ustl::views::Span<HIrqNum const> const hirqnums, Chip const &chip) -> Status {
    return get_desc_table()->init(hirqnums, chip);
}

auto attach(VIrqNum virqnum, Action &action) -> Status {
    return get_desc_table()->attach(virqnum, action);
}

auto attach(VIrqNum const virqnum, Action &action, ustl::Option<LineMode> const mode)
    -> ktl::Result<TriggerMode> {
    return get_desc_table()->attach(virqnum, action, mode);
}

auto detach(Action &action) -> Status {
    return get_desc_table()->detach(action);
}

auto mask(VIrqNum const virqnum) -> Status {
    return get_desc_table()->mask(virqnum);
}

auto unmask(VIrqNum const virqnum) -> Status {
    return get_desc_table()->unmask(virqnum);
}

auto configure(VIrqNum const virqnum, TriggerMode const trigger, Polarity const polarity)
    -> Status {
    return get_desc_table()->configure(virqnum, trigger, polarity);
}

auto trigger_mode(VIrqNum const virqnum) -> ktl::Result<TriggerMode> {
    return get_desc_table()->trigger_mode(virqnum);
}

auto set_affinity(VIrqNum, const CpuMask &targets) -> Status {
    return Status::Ok;
}

auto handle_irq_generic(VIrqNum virqnum) -> IrqReturn {
    ASSERT(virqnum != INVALID_V_IRQ_NUM);
    return get_desc_table()->handle_irq(virqnum);
}

auto spurious_count(VIrqNum virqnum) -> ustl::Option<u64> {
    return get_desc_table()->spurious_count(virqnum);
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