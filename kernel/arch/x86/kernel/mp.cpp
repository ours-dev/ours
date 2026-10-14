#include <ours/arch/apic.hpp>
#include <ours/arch/idle_thread.hpp>
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
#include <ours/task/idle_thread.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>
#include <ours/time/tick_broadcast.hpp>

#include <arch/halt.hpp>
#include <arch/spin_hint.hpp>
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
            case IpiEvent::Timer:
                return IrqVec::IpiTimer;
        }
        panic("Unknown IpiEvent {}", usize(event));
    }
} // namespace
auto arch_mp_send_ipi(CpuNum cpu, IpiEvent event) -> void {
    apic_send_ipi_unicast(cpu, ipi_event_to_irqvec(event), ApicDeliveryMode::Fixed);
}

auto arch_mp_send_ipi_mask(CpuMask const &cpus, IpiEvent event) -> void {
    apic_send_ipi_multicast(cpus, ipi_event_to_irqvec(event), ApicDeliveryMode::Fixed);
}

/// A CPU waiting in MWAIT on its monitor word is woken by writing the word,
/// and needs no IPI (ArchIdleThread::wake()). Its idle loop then looks at its
/// run queue and transitions itself, which is all a reschedule IPI to an idle
/// CPU is for.
auto arch_mp_reschedule(CpuNum cpu) -> void {
    if (ArchIdleThread::wake(cpu)) {
        return;
    }
    apic_send_ipi_unicast(cpu, arch::IrqVec::IpiResched, ApicDeliveryMode::Fixed);
}

auto arch_mp_reschedule_mask(CpuMask const &cpus) -> void {
    auto to_interrupt = CpuMask();
    cpus.for_each([&to_interrupt](CpuNum const cpu) {
        if (!ArchIdleThread::wake(cpu)) {
            to_interrupt.set(cpu);
        }
    });
    if (to_interrupt.none()) {
        return;
    }
    apic_send_ipi_multicast(to_interrupt, arch::IrqVec::IpiResched, ApicDeliveryMode::Fixed);
}

/// The arch half of IpiService::suspend_others(): tell |targets| to stop.
///
/// Nothing more than sending the vector, because there is nothing more to it
/// -- IrqVec::IpiSuspend is handled by x86_handle_ipi_suspend() below, which
/// never returns. It stayed an empty function for as long as nothing sent the
/// event; the handler has always been there waiting for it.
auto arch_mp_suspend(CpuMask targets) -> void {
    arch_mp_send_ipi_mask(targets, IpiEvent::Suspend);
}

/// One bounded send per target, through apic_try_send_ipi_unicast(): its
/// caller is the lockup detector's report, run in an NMI or with the machine
/// already stuck, where an ICR that never goes idle must cost a target, not
/// the report. The vector field means nothing to NMI delivery; it holds the
/// NMI's own vector for want of anything that would.
///
/// Before init_local_apic() there is no APIC to write to, so every target is
/// given up; nothing that early has another CPU to send to anyway.
auto arch_mp_send_nmi(CpuMask const &targets) -> CpuMask {
    if (XAPIC == nullptr) {
        return targets;
    }
    auto unsent = CpuMask{};
    targets.for_each([&unsent](CpuNum cpu) {
        if (!apic_try_send_ipi_unicast(cpu, IrqVec::Nmi, ApicDeliveryMode::NonMaskable)) {
            unsent.set(cpu);
        }
    });
    return unsent;
}

/// The generic IPI says only "look at your mailbox"; IpiService owns what is
/// in it. Nothing arch-specific belongs here, which is the point -- the
/// profiler used to be reached by a second, hardcoded call in the fault
/// dispatcher because there was nowhere else to put it.
auto x86_handle_ipi_generic() -> void {
    IpiService::handle_generic();
}

auto x86_handle_ipi_interrupt() -> void {}

/// No EOI here: x86_dispatch_exception() issues it after this returns, as it
/// does for every other IPI vector. An EOI retires whichever vector is highest
/// in service, not "this one", so a second EOI from here would retire the
/// interrupt this IPI nested in -- letting its source fire again while its
/// handler is still running.
auto x86_handle_ipi_resched() -> void {
    auto const this_cpu = Cpu::id();
    log::trace("IPI-Resched on CPU[{}]", this_cpu);

    task::Thread::Current::preemption_state().add_pending();
}

/// No EOI here, for the reason x86_handle_ipi_resched() gives. The broadcast
/// timer device told this CPU that its timer queue has something due.
auto x86_handle_ipi_timer() -> void {
    time::TickBroadcast::handle_ipi();
}

auto x86_handle_ipi_suspend() -> void {
    auto const this_cpu = Cpu::id();
    log::info("CPU[{}] suspended", this_cpu);

    // Say so before stopping, so that whoever asked can tell when the machine
    // has actually gone quiet rather than guessing at a delay. This is the
    // last thing this CPU ever does, so it is also the last time the bit could
    // be honest.
    Cpu::set_state(this_cpu, Cpu::State::Online, false);

    while (1) {
        arch::local_irq_disable();
        arch::halt();
    }
}

/// How long the bring-up waits after INIT before the startup IPIs, and
/// between the two, as the MP initialisation protocol asks (Intel SDM Vol. 3A,
/// 9.4.4.1): the second SIPI is for a processor that missed the first.
constexpr auto INIT_TO_STARTUP = Milliseconds(10);
constexpr auto STARTUP_TO_STARTUP = Microseconds(200);

namespace {
    /// The two startup IPIs, |vec| naming the trampoline's page.
    CPU_INIT_CODE
    auto send_startup(CpuNum const cpu, arch::IrqVec const vec) -> bool {
        if (!apic_try_send_ipi_unicast(cpu, vec, ApicDeliveryMode::Startup)) {
            return false;
        }
        auto const first = time::SteadyClock::now();
        while (time::SteadyClock::now() - first < STARTUP_TO_STARTUP) {
            arch::spin_hint();
        }
        return apic_try_send_ipi_unicast(cpu, vec, ApicDeliveryMode::Startup);
    }
} // namespace

#ifdef CONFIG_CPU_HOTPLUG
/// `wbinvd` first: a CPU held in INIT, or powered down on a machine that
/// does so, takes its dirty cache lines with it.
auto arch_cpu_halt_offline(std::atomic<bool> &halted) -> void {
    asm volatile("wbinvd" ::: "memory");
    halted.store(true, std::memory_order_release);
    for (;;) {
        arch::local_irq_disable();
        arch::halt();
    }
}

auto arch_deactivate_cpu(CpuNum const cpu) -> Status {
    if (!apic_try_send_ipi_unicast(cpu, arch::IrqVec(), ApicDeliveryMode::Init)) {
        return Status::BadState;
    }
    return Status::Ok;
}

/// The CPU's current thread is still its idle thread, as when it halted, and
/// it starts on that thread's stack from the top: whatever the stack held
/// before is of no use to a CPU INIT has reset.
auto arch_activate_cpu(CpuNum const cpu) -> Status {
    auto *const idler = task::Thread::Current::get(cpu);
    DEBUG_ASSERT(idler != nullptr && idler->is_idle_thread());
    auto const trampoline =
        prepare_bootstrap_area(cpu, idler->kernel_stack().top(), reinterpret_cast<VirtAddr>(idler));

    if (!apic_try_send_ipi_unicast(cpu, arch::IrqVec(), ApicDeliveryMode::Init)) {
        return Status::BadState;
    }
    auto _ = task::Thread::Current::sleep_for(INIT_TO_STARTUP, task::Interruptible::No);
    if (!send_startup(cpu, arch::IrqVec(trampoline >> PAGE_SHIFT))) {
        return Status::BadState;
    }
    return Status::Ok;
}
#else
auto arch_cpu_halt_offline(std::atomic<bool> &) -> void {
    panic("CPU hotplug is not built in (cpu_hotplug=false)");
}

auto arch_deactivate_cpu(CpuNum) -> Status {
    return Status::NotSupported;
}

auto arch_activate_cpu(CpuNum) -> Status {
    return Status::NotSupported;
}
#endif // #ifdef CONFIG_CPU_HOTPLUG

/// The far entry of the 16-bit trampoline; see kernel/arch/x86/kernel/bootstrap16.S,
/// which is what fills in the two arguments.
///
/// |thread| is the idle thread task::init() built for this CPU. It is not
/// needed to run -- init_percpu() below makes Thread::Current::get() answer
/// with it -- but it is the one value the trampoline and the per-CPU area
/// derive independently, so comparing them is a direct check that this CPU
/// landed on the unit it was meant to.
///
/// A CPU coming back from being offline starts here too, its idle thread
/// still Offline: its hardware is set up again as at boot, but for its local
/// APIC timer, which it enrolled once and keeps, and it goes back to its idle
/// thread instead of on through the boot.
extern "C" [[noreturn]] CPU_INIT_CODE USED
auto x86_start_nonboot_cpu(CpuNum cpunum, task::Thread *thread) -> void {
    DEBUG_ASSERT(cpunum != INVALID_CPU_NUM);

    mm::CpuLocal::init_percpu(cpunum);
    DEBUG_ASSERT(task::Thread::Current::get() == thread,
                 "CPU[{}] attached to the wrong per-CPU unit", cpunum);

    auto &idler = task::IdleThread::Current::get();
    auto const coming_back = idler.state() == task::IdleThread::State::Offline;
    if (coming_back) {
        restore_local_apic_percpu();
    } else {
        init_local_apic_percpu();
    }

    x86_init_percpu(cpunum);

    if (coming_back) {
        idler.come_back();
    }

    // Cpu::State::Online is published by start_nonboot_cpu(), not here: it is
    // what x86_bring_aps() spins on before starting the next CPU, so it has
    // to mean "ready for a thread", which is a fact about the generic half of
    // the bring-up rather than the arch half.
    start_nonboot_cpu();
}

/// How long a CPU is given to come online after its startup IPI. A second is
/// far longer than the tens of microseconds it actually takes -- this is the
/// bound that keeps a CPU which will never start from hanging the boot, not a
/// figure anything is expected to approach.
constexpr auto STARTUP_TIMEOUT = Milliseconds(1000);

INIT_CODE
auto x86_bring_aps(CpuMask targets) -> void {
    using task::Thread;
    // Prevent error operation.
    targets.set(0, false);

    targets.for_each([](CpuNum cpu) {
        log::trace("Attemp to wake up CPU[{}]", cpu);
        if (!apic_try_send_ipi_unicast(cpu, arch::IrqVec(), ApicDeliveryMode::Init)) {
            log::error("CPU[{}] did not accept the INIT IPI", cpu);
        }
    });

    BootstrapData *bootstrap_data;
    auto const trampoline = make_bootstrap_area(&bootstrap_data);
    DEBUG_ASSERT(trampoline < 1_mb);

    auto const now = time::SteadyClock::now();
    log::trace("CPU[0]-time={}ns", now.time_since_epoch().count());

    // Not interruptible, so only the timeout wakes it, and that answers Ok.
    [[maybe_unused]]
    auto const slept = Thread::Current::sleep_for(INIT_TO_STARTUP, task::Interruptible::No);
    DEBUG_ASSERT(Status::Ok == slept);
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

        if (!send_startup(cpu, vec)) {
            log::error("CPU[{}] did not accept the startup IPI; carrying on without it", cpu);
            return;
        }

        // Give the CPU a moment to come up before starting to watch for it.
        // Interrupts stay *on* through this and through the wait below: an
        // AP's bring-up is self-contained -- the boot CPU does nothing for it
        // between the SIPI and the Online bit -- so there is nothing here for
        // masking them to protect. There used to be an arch::local_irq_disable()
        // on this line with nothing to re-enable them, so the mask leaked out
        // of this function and left the entire second stage running with
        // interrupts off: reclaim_init_area(), userboot_init(), every thread
        // spawned from labour_routine(), all the way to the first context
        // switch, which restored them by accident. It also meant this CPU
        // could not take a tick while waiting up to a second for an AP.
        //
        // Woken early by a signal, it answers InternalIrqRetry; that only
        // shortens this pause, and the bounded wait below decides either way.
        auto _ = Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::Yes);

        // Wait for |cpu| to join, but do not wait for ever.
        //
        // This spin used to have no way out. A CPU that never starts -- a
        // firmware that reports one the machine does not have, a SIPI that was
        // never accepted -- hung the boot right here, with interrupts disabled
        // and nothing printed, at the one point in the boot where there is
        // nobody left to say so. Giving up costs nothing: everything
        // downstream keys off Cpu::online_mask(), so a CPU that never arrives
        // is simply a CPU the kernel does not have.
        //
        // The bound is a deadline rather than a spin count because there is a
        // clock by now -- the same one this function reads a few lines above.
        auto const started = time::SteadyClock::now();
        while (!Cpu::is_online(cpu)) {
            if (time::SteadyClock::now() - started > STARTUP_TIMEOUT) {
                log::error("CPU[{}] did not come online within {}ms; carrying on without it", cpu,
                           Milliseconds(STARTUP_TIMEOUT).count());
                return;
            }
            arch::spin_hint();
        }
    });
}

} // namespace ours
