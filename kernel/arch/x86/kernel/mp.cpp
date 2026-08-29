#include <arch/halt.hpp>
#include <logz4/log.hpp>
#include <ours/arch/apic.hpp>
#include <ours/arch/mp.hpp>
#include <ours/arch/x86/bootstrap.hpp>
#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/x86/entry.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/arch/x86/idt.hpp>
#include <ours/arch/x86/init.hpp>
#include <ours/cpu.hpp>
#include <ours/ipi_event.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/start.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ustl/fmt/format_to.hpp>

using namespace arch;

namespace ours {
namespace {
    constexpr auto ipi_evnet_to_irqvec(IpiEvent event) -> IrqVec {
        switch (event) {
            case IpiEvent::Resched:
                return IrqVec::IpiResched;
            case IpiEvent::Interrupt:
                return IrqVec::IpiInterrupt;
            case IpiEvent::Suspend:
                return IrqVec::IpiSuspend;
        }

        return IrqVec::IpiGeneric;
    }
} // namespace
auto arch_mp_send_ipi(CpuNum cpu, IpiEvent event) -> void {
    apic_send_ipi_unicast(cpu, ipi_evnet_to_irqvec(event), ApicDeliveryMode::Fixed);
}

auto arch_mp_send_ipi_mask(CpuMask const &cpus, IpiEvent event) -> void {
    apic_send_ipi_mulitcast(cpus, ipi_evnet_to_irqvec(event), ApicDeliveryMode::Fixed);
}

auto arch_mp_reschedule(CpuNum cpu) -> void {
    apic_send_ipi_unicast(cpu, arch::IrqVec::IpiResched, ApicDeliveryMode::Fixed);
}

auto arch_mp_reschedule_mask(CpuMask const &cpus) -> void {
    apic_send_ipi_mulitcast(cpus, arch::IrqVec::IpiResched, ApicDeliveryMode::Fixed);
}

auto arch_mp_suspend(CpuMask targets) -> void {}

auto x86_handle_ipi_generic() -> void {}

auto x86_handle_ipi_interrupt() -> void {}

auto x86_handle_ipi_resched() -> void {
    apic_issue_eoi();

    auto const this_cpu = Cpu::id();
    log::trace("IPI-Resched on CPU[{}]", this_cpu);

    task::Thread::Current::preemption_state().add_pending();
}

auto x86_handle_ipi_suspend() -> void {
    auto const this_cpu = Cpu::id();
    log::trace("IPI-Suspend on CPU[{}]", this_cpu);

    while (1) {
        arch::local_irq_disable();
        arch::halt();
    }
}

auto arch_activate_cpu(CpuNum) -> Status {
    return Status::Unimplemented;
}

auto arch_deactivate_cpu(CpuNum) -> Status {
    return Status::Unimplemented;
}

/// Called once in kernel/arch/x86/entry/bootstrap16.S
NO_MANGLE NO_RETURN INIT_CODE USED auto x86_start_nonboot_cpu(CpuNum cpunum) -> void {
    DEBUG_ASSERT(cpunum != INVALID_CPU_NUM);

    mm::CpuLocal::init_percpu(cpunum);

    Cpu::set_state(Cpu::State::Online);

    init_local_apic_percpu();

    x86_init_percpu(cpunum);

    start_nonboot_cpu();
}

INIT_CODE
auto x86_bring_aps(CpuMask targets) -> void {
    using task::Thread;
    // Prevent error operation.
    targets.set(0, false);

    targets.for_each([](CpuNum cpu) {
        log::trace("Attemp to wake up CPU[{}]", cpu);
        apic_send_ipi_unicast(cpu, arch::IrqVec(), ApicDeliveryMode::Init);
    });

    BootstrapData *bootstrap_data;
    auto const trampoline = make_bootstrap_area(&bootstrap_data);
    DEBUG_ASSERT(trampoline < MB(1));

    auto const now = time::SteadyClock::now();
    log::trace("CPU[0]-time={}ns", now.time_since_epoch().count());

    Thread::Current::sleep_for(Milliseconds(10), task::Interruptible::No);
    auto const vec = arch::IrqVec(trampoline >> PAGE_SHIFT);

    DEBUG_ASSERT(MAX_CPU < 1000'000'000'000'000'000);
    targets.for_each([bootstrap_data, vec](CpuNum cpu) {
        log::trace("Attempt to wake up CPU[{}]", cpu);

        auto const current_thread = Thread::Current::get(cpu);
        DEBUG_ASSERT(current_thread);

        auto const initial_stack = current_thread->kernel_stack().top();
        log::trace("CPU[{}]=(s: {:X})", cpu, initial_stack);

        bootstrap_data->cpunum = cpu;
        bootstrap_data->stack_top = initial_stack;

        // What bootstrap16.S actually reads. It indexes by cpunum - 1, since
        // the boot CPU never goes through the trampoline.
        DEBUG_ASSERT(cpu > 0 && cpu < MP_MAX_CPUS);
        auto const slot = usize(cpu - 1) * 2;
        bootstrap_data->percpu[slot] = initial_stack;
        bootstrap_data->percpu[slot + 1] = reinterpret_cast<VirtAddr>(current_thread);

        apic_send_ipi_unicast(cpu, vec, ApicDeliveryMode::Startup);

        arch::local_irq_disable();

        Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::Yes);

        // Waiting until |cpu| is online.
        while (!Cpu::is_online(cpu))
            ;
    });
}

} // namespace ours
