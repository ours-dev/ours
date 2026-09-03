#include <ours/arch/mp.hpp>
#include <ours/ipi_service.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/task/scheduler.hpp>

namespace ours {
auto IpiService::init() -> void {}

auto IpiService::send_resched(CpuNum cpu) -> void {
    if (!task::Scheduler::peek_active_mask().test(cpu)) {
        return;
    }

    arch_mp_reschedule(cpu);
}

auto IpiService::send_resched(CpuMask const &mask) -> void {
    auto const local_cpu = Cpu::id();

    // The local CPU is never a target: a reschedule request for this CPU is a
    // pending-preemption bit, not an interrupt to itself, and every caller has
    // already recorded that bit before coming here. Inactive CPUs cannot take
    // a reschedule either. What is left is what gets the IPI -- |targets|,
    // not |mask|: this used to filter into |targets| and then test and send
    // |mask| anyway, so every wakeup on the boot CPU interrupted itself
    // ("IPI-Resched on CPU[0]" in the boot log) and inactive CPUs were IPI'd.
    auto targets = mask;
    targets.set(local_cpu, 0);
    targets &= task::Scheduler::peek_active_mask();

    if (!targets.count()) {
        return;
    }

    arch_mp_reschedule_mask(targets);
}

} // namespace ours