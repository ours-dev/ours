#include <arch/halt.hpp>
#include <arch/interrupt.hpp>
#include <logz4/log.hpp>
#include <ours/arch/apic.hpp>
#include <ours/arch/x86/faults.hpp>
#include <ours/platform/init.hpp>
#include <ours/platform/interrupt.hpp>
#include <ours/task/thread.hpp>

namespace ours {
namespace {
    FORCE_INLINE auto irqvec_to_irqnum(arch::IrqVec vector) -> HIrqNum {
        DEBUG_ASSERT(static_cast<HIrqNum>(vector) >=
                     static_cast<HIrqNum>(arch::IrqVec::PlatformIrqMin));
        return static_cast<HIrqNum>(vector) - static_cast<HIrqNum>(arch::IrqVec::PlatformIrqMin);
    }

    NO_RETURN FORCE_INLINE auto panic_exception(arch::IrqFrame *frame, char const *msg) -> void {
        log::trace("{}: vector[{}] panicked", msg, u32(frame->vector));

        while (1) {
            arch::halt();
        }
    }

    auto x86_dispatch_exception(arch::IrqVec vector, arch::IrqFrame *frame) -> void {
        using namespace arch;
        if (vector == IrqVec::PageFault) {
            if (x86_handle_page_fault(frame) != Status::Ok) {
                panic_exception(frame, "Unhandled page fault");
            }
            return;
        }

        switch (vector) {
            case IrqVec::DivideError:
                log::info("#DE");
                break;
            case IrqVec::Debug:
                log::info("#DB");
                break;
            case IrqVec::Nmi:
                log::info("#NMI");
                break;
            case IrqVec::Breakpoint:
                log::info("#BK");
                break;
            case IrqVec::Overflow:
                log::info("#OF");
                break;
            case IrqVec::BoundRangeExceeded:
                log::info("#BE");
                break;
            case IrqVec::InvalidOpcode:
                log::info("#IO");
                break;
            case IrqVec::DeviceNotAvailable:
                log::info("#DNA");
                break;
            case IrqVec::DoubleFault:
                panic_exception(frame, "Double fault");
                break;
            case IrqVec::CoprocessorSegmentOverrun:
                log::info("#CSO");
                break;
            case IrqVec::InvalidTss:
                log::info("#IT");
                break;
            case IrqVec::SegmentNotPresent:
                log::info("#SNP");
                break;
            case IrqVec::StackFaultException:
                log::info("#SF");
                break;
            case IrqVec::GeneralProtection:
                log::info("#GP");
                break;
            case IrqVec::SpuriousInterrupt:
                log::info("#SP");
                break;
            case IrqVec::X87FloatingPoint:
                log::info("#X86FP");
                break;
            case IrqVec::AlignmentCheck:
                log::info("#AC");
                break;
            case IrqVec::MachineCheck:
                log::info("#MC");
                break;
            case IrqVec::SimdFloatingPoint:
                log::info("#SIMDFP");
                break;
            case IrqVec::Virtualizatoin:
                log::info("#V");
                break;
            case IrqVec::ControlProtection:
                log::info("#CP");
                break;
            case IrqVec::VmmCommunicationException:
                log::info("#VC");
                break;

            case IrqVec::PlatformIrqMin... IrqVec::PlatformIrqMax:
                platform_handle_irq(irqvec_to_irqnum(vector), frame);
                // TODO(SmallHuaZi) Replace it with IrqChip::issue_eoi()
                apic_issue_eoi();
                break;

            case IrqVec::ApicTimer:
                apic_handle_timer_irq();
                break;

            case IrqVec::ApicSpurious:
                log::info("APIC spurious interrupt");
                break;

            case IrqVec::ApicError:
                log::info("APIC error interrupt");
                apic_issue_eoi();
                break;

            case IrqVec::ApicPmi:
                log::info("APIC performance-monitoring interrupt");
                apic_issue_eoi();
                break;

            case IrqVec::IpiGeneric:
                x86_handle_ipi_generic();
                apic_issue_eoi();
                break;

            case IrqVec::IpiInterrupt:
                x86_handle_ipi_interrupt();
                apic_issue_eoi();
                break;

            case IrqVec::IpiResched:
                x86_handle_ipi_resched();
                apic_issue_eoi();
                break;

            case IrqVec::IpiSuspend:
                x86_handle_ipi_suspend();
                // Never return.
                break;

            default:
                panic_exception(frame, "Unhandled interrupt vector");
        }
    }

    FORCE_INLINE auto is_from_uspace(arch::IrqFrame *frame) -> bool {
        return (frame->cs & 0x3) != 0;
    }
} // namespace

NO_MANGLE
auto arch_handle_exception(arch::IrqFrame *frame) -> void {
    auto const irqnum = HIrqNum(frame->vector);
    platform_start_handling_irq(irqnum, frame);

    x86_dispatch_exception(frame->vector, frame);

    using task::Thread;
    if (is_from_uspace(frame)) {
        log::trace("Exception[{}] from user space", to_string(frame->vector));
        Thread::Current::process_pending_signals();
    }

    auto const need_preempt = platform_finish_handling_irq(irqnum, frame);
    if (need_preempt) {
        Thread::Current::preempt();
    }
}

} // namespace ours
