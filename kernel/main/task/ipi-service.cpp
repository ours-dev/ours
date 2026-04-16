#include <ours/ipi-service.hpp>

#include <ours/arch/mp.hpp>
#include <ours/task/scheduler.hpp>

#include <ours/mem/cpu-local.hpp>

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

        auto targets= mask;
        // Clear local CPU and inactive CPUs.
        targets.set(local_cpu, 0);
        targets &= task::Scheduler::peek_active_mask();

        if (!mask.count()) {
            return;
        }

        arch_mp_reschedule_mask(mask);
    }

} // namespace ours