#include <ktl/new.hpp>
#include <ours/irq/irq_dispatcher.hpp>
#include <ours/irq/irq_observer.hpp>
#include <ours/irq/mod.hpp>
#include <ours/task/thread.hpp>

namespace ours::irq {
FORCE_INLINE static auto get_irq_dispatcher() -> IrqDispatcher * {
    static IrqDispatcher g_irq_dispatcher;
    return &g_irq_dispatcher;
}

auto init_early(usize num_irqs) -> void {
    auto status = get_irq_dispatcher()->init(num_irqs);
    ASSERT(Status::Ok == status);
}

auto request_irq(VIrqNum virqnum, IrqHandler handler, IrqFlags flags, char const *name) -> Status {
    ASSERT(virqnum != INVALID_V_IRQ_NUM);
    return get_irq_dispatcher()->request_irq(virqnum, handler, flags, name);
}

auto release_irq(VIrqNum virqnum) -> Status {
    ASSERT(virqnum != INVALID_V_IRQ_NUM);
    return get_irq_dispatcher()->release_irq(virqnum);
}

auto set_affinity(VIrqNum, const CpuMask &targets) -> Status {
    return Status::Ok;
}

auto handle_irq_generic(VIrqNum virqnum) -> IrqReturn {
    ASSERT(virqnum != INVALID_V_IRQ_NUM);
    return get_irq_dispatcher()->handle_irq(virqnum);
}

auto handle_irq_preamble(HIrqNum irqnum, arch::IrqFrame *frame) -> void {
    auto &preemption_state = task::Thread::Current::preemption_state();
    preemption_state.disable_eager_reschedule();
    preemption_state.disable_preemption();
}

auto handle_irq_postamble() -> bool {
    auto &preemption_state = task::Thread::Current::preemption_state();
    preemption_state.enable_preemption();
    preemption_state.enable_eager_reschedule();

    return preemption_state.is_preemption_enabled();
}

} // namespace ours::irq