#include <ours/assert.hpp>
#include <ours/irq/entry.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/task/thread.hpp>

#include <atomic>

#include <arch/interrupt.hpp>

namespace ours::irq {
namespace {
    CPU_LOCAL
    arch::IrqFrame *CURRENT_FRAME;

    std::atomic<RootHandler> ROOT_HANDLER{nullptr};
} // namespace

auto enter_trap(arch::IrqFrame *const frame) -> arch::IrqFrame * {
    ASSERT(arch::irqs_disabled());
    ASSERT(frame != nullptr);
    auto *const previous = mm::CpuLocal::read(CURRENT_FRAME);
    mm::CpuLocal::write(CURRENT_FRAME, frame);
    auto &preemption = task::Thread::Current::preemption_state();
    preemption.disable_eager_reschedule();
    preemption.disable_preemption();
    return previous;
}

auto leave_trap(arch::IrqFrame *const previous) -> bool {
    ASSERT(arch::irqs_disabled());
    mm::CpuLocal::write(CURRENT_FRAME, previous);
    auto &preemption = task::Thread::Current::preemption_state();
    // Remote requests may flush while the preemption count is still raised.
    // Then drop that count without scheduling inside entry bookkeeping.
    preemption.enable_eager_reschedule();
    preemption.enable_preemption_delay_resched();
    return preemption.is_preemption_enabled() && preemption.take_pending_local();
}

auto current_frame() -> arch::IrqFrame * {
    return mm::CpuLocal::read(CURRENT_FRAME);
}

auto set_root_handler(RootHandler const handler) -> void {
    ASSERT(handler != nullptr, "The root interrupt handler is null");
    auto expected = RootHandler{nullptr};
    [[maybe_unused]]
    auto const installed = ROOT_HANDLER.compare_exchange_strong(
        expected, handler, std::memory_order_release, std::memory_order_relaxed);
    ASSERT(installed, "A root interrupt handler is already installed");
}

auto handle_root_irq(arch::IrqFrame *const frame) -> void {
    auto *const handler = ROOT_HANDLER.load(std::memory_order_acquire);
    ASSERT(handler != nullptr, "An interrupt reached the root handler before one was installed");
    handler(frame);
}
} // namespace ours::irq
