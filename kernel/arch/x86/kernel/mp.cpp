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
#include <ours/ipi_service.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/start.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <arch/halt.hpp>
#include <logz4/log.hpp>
#include <ustl/fmt/format_to.hpp>

using namespace arch;

namespace ours {
namespace {
    /// Total on purpose, with no fallthrough after the switch: a `return
    /// IrqVec::IpiGeneric` sat there and made Generic the answer for any
    /// event the switch forgot, which is a silent misdelivery rather than a
    /// build error. With every enumerator named, adding one to IpiEvent now
    /// fails to compile here, which is where it should fail.
    constexpr auto ipi_event_to_irqvec(IpiEvent event) -> IrqVec {
        switch (event) {
            case IpiEvent::Generic:
                return IrqVec::IpiGeneric;
            case IpiEvent::Resched:
                return IrqVec::IpiResched;
            case IpiEvent::Interrupt:
                return IrqVec::IpiInterrupt;
            case IpiEvent::Suspend:
                return IrqVec::IpiSuspend;
        }
        panic("Unknown IpiEvent {}", usize(event));
    }
} // namespace
auto arch_mp_send_ipi(CpuNum cpu, IpiEvent event) -> void {
    apic_send_ipi_unicast(cpu, ipi_event_to_irqvec(event), ApicDeliveryMode::Fixed);
}

auto arch_mp_send_ipi_mask(CpuMask const &cpus, IpiEvent event) -> void {
    apic_send_ipi_mulitcast(cpus, ipi_event_to_irqvec(event), ApicDeliveryMode::Fixed);
}

auto arch_mp_reschedule(CpuNum cpu) -> void {
    apic_send_ipi_unicast(cpu, arch::IrqVec::IpiResched, ApicDeliveryMode::Fixed);
}

auto arch_mp_reschedule_mask(CpuMask const &cpus) -> void {
    apic_send_ipi_mulitcast(cpus, arch::IrqVec::IpiResched, ApicDeliveryMode::Fixed);
}

auto arch_mp_suspend(CpuMask targets) -> void {}

/// The generic IPI says only "look at your mailbox"; IpiService owns what is
/// in it. Nothing arch-specific belongs here, which is the point -- the
/// profiler used to be reached by a second, hardcoded call in the fault
/// dispatcher because there was nowhere else to put it.
auto x86_handle_ipi_generic() -> void {
    IpiService::handle_generic();
}

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

/// The far entry of the 16-bit trampoline; see kernel/arch/x86/kernel/bootstrap16.S,
/// which is what fills in the two arguments.
///
/// |thread| is the idle thread task::init() built for this CPU. It is not
/// needed to run -- init_percpu() below makes Thread::Current::get() answer
/// with it -- but it is the one value the trampoline and the per-CPU area
/// derive independently, so comparing them is a direct check that this CPU
/// landed on the unit it was meant to.
extern "C" [[noreturn]] INIT_CODE USED
auto x86_start_nonboot_cpu(CpuNum cpunum, task::Thread *thread) -> void {
    DEBUG_ASSERT(cpunum != INVALID_CPU_NUM);

    mm::CpuLocal::init_percpu(cpunum);
    DEBUG_ASSERT(task::Thread::Current::get() == thread,
                 "CPU[{}] attached to the wrong per-CPU unit", cpunum);

    init_local_apic_percpu();

    x86_init_percpu(cpunum);

    // Cpu::State::Online is published by start_nonboot_cpu(), not here: it is
    // what x86_bring_aps() spins on before starting the next CPU, so it has
    // to mean "ready for a thread", which is a fact about the generic half of
    // the bring-up rather than the arch half.
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

    targets.for_each([bootstrap_data, vec](CpuNum cpu) {
        log::trace("Attempt to wake up CPU[{}]", cpu);

        auto const current_thread = Thread::Current::get(cpu);
        DEBUG_ASSERT(current_thread);

        auto const initial_stack = current_thread->kernel_stack().top();
        log::trace("CPU[{}]=(s: {:X})", cpu, initial_stack);

        // The one live slot in the trampoline page. Safe to overwrite per CPU
        // because the loop below waits for this one to come online before it
        // reaches this line again.
        bootstrap_data->cpunum = cpu;
        bootstrap_data->stack_top = initial_stack;
        bootstrap_data->thread = reinterpret_cast<VirtAddr>(current_thread);

        apic_send_ipi_unicast(cpu, vec, ApicDeliveryMode::Startup);

        arch::local_irq_disable();

        Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::Yes);

        // Waiting until |cpu| is online.
        while (!Cpu::is_online(cpu))
            ;
    });
}

} // namespace ours
