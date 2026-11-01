#include <ours/irq/action.hpp>
#include <ours/irq/desc_table.hpp>
#include <ours/irq/mod.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/task/thread.hpp>

#include <atomic>

#include <heap/new.hpp>

namespace ours::irq {
namespace {
    // Trap frame of the interrupt currently being handled on this CPU, or
    // nullptr outside interrupt context. Instrumentation (e.g. the sampling
    // profiler's tick hook) reads it to reach the interrupted context.
    CPU_LOCAL
    arch::IrqFrame *CURRENT_FRAME;

    // The root controller's handler. Written once by set_root_handler() on the
    // boot CPU, in the controller's start-up hook, which runs before that CPU
    // enables interrupts and before any other CPU starts; read in interrupt
    // context on every CPU with no lock. That order is what keeps a CPU from
    // reading it too early. The release store and the acquire load make the
    // line table, built before the store, visible to whoever reads a handler.
    std::atomic<RootHandler> ROOT_HANDLER{nullptr};
} // namespace

auto set_root_handler(RootHandler const handler) -> void {
    ASSERT(handler != nullptr, "The root interrupt handler is null");
    auto expected = RootHandler{nullptr};
    [[maybe_unused]]
    auto const installed = ROOT_HANDLER.compare_exchange_strong(
        expected, handler, std::memory_order_release, std::memory_order_relaxed);
    ASSERT(installed, "A root interrupt handler is already installed");
}

auto handle_root_irq(::arch::IrqFrame *const frame) -> void {
    auto *const handler = ROOT_HANDLER.load(std::memory_order_acquire);
    ASSERT(handler != nullptr, "An interrupt reached the root handler before one was installed");
    handler(frame);
}

auto current_frame() -> arch::IrqFrame * {
    return mm::CpuLocal::read(CURRENT_FRAME);
}
FORCE_INLINE
static auto get_desc_table() -> DescTable * {
    static DescTable TABLE;
    return &TABLE;
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

auto finalize_oneshot(VIrqNum const virqnum, Action const &action) -> void {
    get_desc_table()->finalize_oneshot(virqnum, action);
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

auto is_masked_for_test(VIrqNum const virqnum) -> ktl::Result<bool> {
    return get_desc_table()->is_masked(virqnum);
}

auto set_affinity(VIrqNum, CpuMask const &) -> Status {
    return Status::NotSupported;
}

auto virq_of_hwirq(HIrqNum const hirqnum) -> VIrqNum {
    return get_desc_table()->virq_of(hirqnum);
}

auto hwirq_of_virq(VIrqNum const virqnum) -> HIrqNum {
    return get_desc_table()->hwirq_of(virqnum);
}

auto handle_irq_generic(HIrqNum const hirqnum) -> bool {
    ASSERT(hirqnum != INVALID_H_IRQ_NUM);
    return get_desc_table()->handle_irq(hirqnum, Eoi::Send).is_some();
}

/// A played interrupt names its line by virq, as every caller outside the
/// interrupt entry does, so it is turned into the hardware number first; a
/// number that is no line becomes INVALID_H_IRQ_NUM, which has no line.
auto handle_irq_for_test(VIrqNum const virqnum) -> IrqReturn {
    auto *const table = get_desc_table();
    auto const result = table->handle_irq(table->hwirq_of(virqnum), Eoi::Skip);
    if (result.is_none()) {
        return IrqReturn::None;
    }
    return *result;
}

auto spurious_count(VIrqNum virqnum) -> ustl::Option<u64> {
    return get_desc_table()->spurious_count(virqnum);
}

auto is_kernel_owned(VIrqNum const virqnum) -> bool {
    return get_desc_table()->is_kernel_owned(virqnum);
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
auto handle_irq_preamble(arch::IrqFrame *frame) -> void {
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