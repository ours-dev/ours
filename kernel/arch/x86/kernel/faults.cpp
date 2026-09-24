#include <ours/arch/apic.hpp>
#include <ours/arch/x86/faults.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/platform/init.hpp>
#include <ours/platform/interrupt.hpp>
#include <ours/task/thread.hpp>

#include <arch/halt.hpp>
#include <arch/interrupt.hpp>
#include <arch/x86/system.hpp>
#include <logz4/log.hpp>

namespace ours {
namespace {
    FORCE_INLINE
    auto irqvec_to_irqnum(arch::IrqVec vector) -> HIrqNum {
        DEBUG_ASSERT(static_cast<HIrqNum>(vector) >=
                     static_cast<HIrqNum>(arch::IrqVec::PlatformIrqMin));
        return static_cast<HIrqNum>(vector) - static_cast<HIrqNum>(arch::IrqVec::PlatformIrqMin);
    }

    /// Reports a CPU exception the kernel cannot recover from, and stops.
    ///
    /// The register dump is the whole point. An exception taken in kernel mode
    /// has no other witness: there is no debugger attached, no core to inspect
    /// afterwards, and the halt below means nothing further will be printed. So
    /// everything needed to place the fault -- the instruction it happened on,
    /// the error code the CPU pushed, and the register that most likely holds
    /// the bad value -- has to go out on the serial line here or be lost.
    ///
    /// `rip` is printed as an image-relative offset as well as an absolute
    /// address, because the kernel is a static PIE relocated at load time: the
    /// absolute address differs from run to run, while the offset is what can be
    /// looked up directly in `llvm-objdump -d vmours`.
    [[noreturn]]
    static auto panic_exception(arch::IrqFrame *frame, char const *msg) -> void {
        log::error("{}: vector={}, error_code={:#x}", msg, u32(frame->vector), frame->error_code);

        // The faulting address, which the dump did not carry and which is the
        // first thing anyone reading it wants. CR2 is only meaningful for a
        // page fault, and only until the next one, which cannot happen here --
        // nothing below faults and the function ends in a halt loop.
        //
        // For an instruction-fetch fault it equals rip and says nothing new;
        // for every data fault it is the whole question, and a dump that
        // printed rip alone left the reader to guess which address the
        // instruction was reaching for.
        if (frame->vector == arch::IrqVec::PageFault) {
            log::error("  cr2={:#x}", arch::Cr2::read().get<arch::Cr2::Address>());
        }
        log::error("  rip={:#x} (+{:#x})  cs={:#x}  rsp={:#x}", frame->rip,
                   frame->rip - VirtAddr(mm::IMAGE_START), frame->cs, frame->usp);
        log::error("  rax={:#x}  rbx={:#x}  rcx={:#x}  rdx={:#x}", frame->ax, frame->bx, frame->cx,
                   frame->dx);
        log::error("  rsi={:#x}  rdi={:#x}  rbp={:#x}", frame->si, frame->di, frame->bp);
        log::error("  r8={:#x}  r9={:#x}  r10={:#x}  r11={:#x}", frame->r8, frame->r9, frame->r10,
                   frame->r11);
        log::error("  r12={:#x}  r13={:#x}  r14={:#x}  r15={:#x}", frame->r12, frame->r13,
                   frame->r14, frame->r15);

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

        // Everything below that panics is a *fault*: the CPU pushes the address
        // of the instruction that raised it, so returning re-executes that same
        // instruction and raises it again, forever. Logging a mnemonic and
        // breaking -- which is what these cases used to do -- therefore turns
        // any one of them into a silent hang that buries the original cause
        // under an endless stream of identical lines.
        //
        // The three that stay non-fatal are the ones where returning is
        // meaningful: #DB and #BP are traps, so the pushed address is already
        // past the instruction, and an NMI is external rather than a
        // consequence of the interrupted code.
        switch (vector) {
            case IrqVec::DivideError:
                panic_exception(frame, "#DE Divide error");
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
                panic_exception(frame, "#BR Bound range exceeded");
            case IrqVec::InvalidOpcode:
                panic_exception(frame, "#UD Invalid opcode");
            case IrqVec::DeviceNotAvailable:
                panic_exception(frame, "#NM Device not available");
            case IrqVec::DoubleFault:
                panic_exception(frame, "Double fault");
                break;
            case IrqVec::CoprocessorSegmentOverrun:
                panic_exception(frame, "#CSO Coprocessor segment overrun");
            case IrqVec::InvalidTss:
                panic_exception(frame, "#TS Invalid TSS");
            case IrqVec::SegmentNotPresent:
                panic_exception(frame, "#NP Segment not present");
            case IrqVec::StackFaultException:
                panic_exception(frame, "#SS Stack fault");
            case IrqVec::GeneralProtection:
                panic_exception(frame, "#GP General protection fault");
            case IrqVec::SpuriousInterrupt:
                log::info("#SP");
                break;
            case IrqVec::X87FloatingPoint:
                panic_exception(frame, "#MF x87 floating-point error");
            case IrqVec::AlignmentCheck:
                panic_exception(frame, "#AC Alignment check");
            case IrqVec::MachineCheck:
                panic_exception(frame, "#MC Machine check");
            case IrqVec::SimdFloatingPoint:
                panic_exception(frame, "#XM SIMD floating-point error");
            case IrqVec::Virtualizatoin:
                panic_exception(frame, "#VE Virtualization exception");
            case IrqVec::ControlProtection:
                panic_exception(frame, "#CP Control protection fault");
            case IrqVec::VmmCommunicationException:
                panic_exception(frame, "#VC VMM communication exception");

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
                x86_handle_pmi(frame);
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

    FORCE_INLINE
    auto is_from_uspace(arch::IrqFrame *frame) -> bool {
        return (frame->cs & 0x3) != 0;
    }
} // namespace

/// The pending-signal check on the way out of a system call, called by
/// x86_syscall (syscall.S) after the dispatcher returns and before the saved
/// user registers are popped. Interrupts are off, as process_pending_signals()
/// requires. It may not return: a pending Kill exits the thread here.
///
/// Zircon passes the saved registers along so that a debugger can read a
/// suspended thread's state; nothing here reads them yet, so none are passed.
/// Zircon counterpart: x86_syscall_process_pending_signals() in
/// kernel/arch/x86/exceptions_c.cc.
extern "C" auto x86_syscall_process_pending_signals() -> void {
    task::Thread::Current::process_pending_signals();
}

extern "C" auto arch_handle_exception(arch::IrqFrame *frame) -> void {
    using task::Thread;
    auto const vector = frame->vector;

    // NMI, #DF and #MC take none of the bookkeeping below. They can arrive
    // anywhere -- inside another interrupt's handler, between the preamble
    // and postamble of one, with a per-CPU structure half updated -- and the
    // bookkeeping is exactly what they must not touch:
    //
    //  - the preamble and postamble write the per-CPU CURRENT_FRAME and move
    //    the preempt count. An NMI nested in an IRQ used to run the full set,
    //    so its postamble cleared CURRENT_FRAME to null under the IRQ still on
    //    the stack, which then carried on believing it had a frame;
    //  - preempting would switch threads while NMIs are still masked, which
    //    they stay until the next iretq, and that iretq would then belong to
    //    some other thread's return path;
    //  - processing signals could exit the thread from inside an NMI.
    //
    // #DF and #MC stop the kernel in x86_dispatch_exception() and never
    // return; they skip the bookkeeping because nothing on a double-fault or
    // machine-check path should depend on per-CPU state being consistent.
    // (The dedicated IST stacks such exceptions want are not configured yet:
    // docs/repairs/260923/x86-kernel.md, #1.)
    //
    // Zircon gives the NMI its own handler (x86_nmi_handler()) and asserts
    // that x86_exception_handler() never sees one.
    if (vector == arch::IrqVec::Nmi || vector == arch::IrqVec::DoubleFault ||
        vector == arch::IrqVec::MachineCheck) {
        x86_dispatch_exception(vector, frame);
        return;
    }

    auto const irqnum = HIrqNum(vector);
    auto const from_user = is_from_uspace(frame);

    platform_start_handling_irq(irqnum, frame);
    x86_dispatch_exception(vector, frame);
    auto const need_preempt = platform_finish_handling_irq(irqnum, frame);

    // The order is Zircon's x86_exception_handler(): finish the interrupt,
    // then preempt, then signals. Signals come last because the next change
    // to them lets a pending Kill exit the thread from here, and a thread
    // that exits must not leave behind the preempt count the preamble raised
    // or a CURRENT_FRAME pointing into its dead stack -- so the interrupt has
    // to be fully put away first. They also come after the preemption, so
    // that a signal sent while this thread was switched out is seen before it
    // returns to ring 3 rather than one interrupt later.
    if (need_preempt) {
        Thread::Current::preempt();
    }
    if (from_user) {
        log::trace("Exception[{}] from user space", to_string(vector));
        Thread::Current::process_pending_signals();
    }

    // Still off, as the stub's iretq path requires: nothing above may leave
    // interrupts enabled on its way back here.
    DEBUG_ASSERT(arch::irqs_disabled(), "interrupts enabled on the way out of exception {}",
                 to_string(vector));
}

} // namespace ours
