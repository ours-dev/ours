#include <ours/arch/apic.hpp>
#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/x86/faults.hpp>
#include <ours/irq/entry.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/platform/init.hpp>
#include <ours/task/exception.hpp>
#include <ours/task/thread.hpp>

#include <arch/auto_interrupt_enabler.hpp>
#include <arch/halt.hpp>
#include <arch/interrupt.hpp>
#include <arch/x86/system.hpp>
#include <lockup_detector/lockup_detector.hpp>
#include <logz4/log.hpp>
#include <ustl/option.hpp>

namespace ours {
namespace {
    /// The interrupted registers, at error level, five lines: rip as an
    /// image offset and cs and rsp beside it, then the fifteen general
    /// registers. A fault's report prints them, and so does each CPU's block
    /// of the lockup detector's dump, so the two read alike.
    auto log_registers(arch::IrqFrame const &frame) -> void {
        log::error("  rip={:#x} (+{:#x})  cs={:#x}  rsp={:#x}", frame.rip,
                   frame.rip - VirtAddr(mm::IMAGE_START), frame.cs, frame.usp);
        log::error("  rax={:#x}  rbx={:#x}  rcx={:#x}  rdx={:#x}", frame.ax, frame.bx, frame.cx,
                   frame.dx);
        log::error("  rsi={:#x}  rdi={:#x}  rbp={:#x}", frame.si, frame.di, frame.bp);
        log::error("  r8={:#x}  r9={:#x}  r10={:#x}  r11={:#x}", frame.r8, frame.r9, frame.r10,
                   frame.r11);
        log::error("  r12={:#x}  r13={:#x}  r14={:#x}  r15={:#x}", frame.r12, frame.r13, frame.r14,
                   frame.r15);
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
        log_registers(*frame);

        while (1) {
            arch::halt();
        }
    }

    FORCE_INLINE
    auto is_from_uspace(arch::IrqFrame const *frame) -> bool {
        return (frame->cs & 0x3) != 0;
    }

    /// The kind of user exception |vector| is, when a thread in ring 3 raises
    /// it and the kernel should kill its process for it; none for a vector
    /// that is not the thread's doing.
    ///
    /// #UD is its own kind, #PF is FatalPageFault once the address space has
    /// failed to resolve it, #DB and #BP are breakpoints, and the rest of the
    /// faults a program can cause are General. A ring-3 #CP is among them: it
    /// is the program's, so it kills the program rather than the kernel. CET
    /// is not enabled, so no #CP can arrive yet.
    ///
    /// #BP and #OF cannot actually arrive from ring 3 today: every IDT gate
    /// has DPL 0 (idt.cpp), so `int3` from ring 3 is a #GP, and INTO is #UD
    /// in 64-bit mode. They are mapped for completeness.
    ///
    /// Not here, and fatal whatever the ring: #NM, #DF, #CSO, #TS, #MC, #VE,
    /// #VC -- nothing a program can cause short of a kernel bug or broken
    /// hardware. Not here either: NMI, which is not the thread's doing; it
    /// goes to the lockup detector, whichever ring it interrupted.
    auto user_exception_type(arch::IrqVec const vector) -> ustl::Option<task::ExceptionType> {
        using arch::IrqVec;
        using task::ExceptionType;
        switch (vector) {
            case IrqVec::DivideError:
            case IrqVec::Overflow:
            case IrqVec::BoundRangeExceeded:
            case IrqVec::SegmentNotPresent:
            case IrqVec::StackFaultException:
            case IrqVec::GeneralProtection:
            case IrqVec::X87FloatingPoint:
            case IrqVec::AlignmentCheck:
            case IrqVec::SimdFloatingPoint:
            case IrqVec::ControlProtection:
                return ustl::some(ExceptionType::General);
            case IrqVec::InvalidOpcode:
                return ustl::some(ExceptionType::UndefinedInstruction);
            case IrqVec::PageFault:
                return ustl::some(ExceptionType::FatalPageFault);
            case IrqVec::Debug:
                // A program can set TF with popf and take one after every
                // instruction; with no debugger to hand it to, letting it
                // return would let a program flood the console at will.
                return ustl::some(ExceptionType::HwBreakpoint);
            case IrqVec::Breakpoint:
                return ustl::some(ExceptionType::SwBreakpoint);
            default:
                return ustl::none();
        }
    }

    auto make_user_exception(task::ExceptionType const type, arch::IrqFrame const *const frame,
                             VirtAddr const fault_address) -> task::ExceptionContext {
        return task::ExceptionContext{
            .type = type,
            .vector_name = arch::to_string(frame->vector),
            .error_code = frame->error_code,
            .ip = frame->ip,
            .sp = frame->usp,
            .fault_address = fault_address,
            .synth_code = 0,
            .synth_data = 0,
            .frame = frame,
        };
    }

    /// Handles |vector|. Returns true when it was an exception a user thread
    /// raised and the kernel will not resolve, with its description in
    /// |user_exception|; the caller delivers it once the interrupt is put away.
    auto x86_dispatch_exception(arch::IrqVec vector, arch::IrqFrame *frame,
                                task::ExceptionContext *user_exception) -> bool {
        using namespace arch;
        if (vector == IrqVec::PageFault) {
            // CR2 first, before anything that could itself fault and replace
            // it: interrupts are still off here, and the handler below may
            // well take a fault of its own while it looks for the page.
            auto const cr2 = Cr2::read().get<Cr2::Address>();
            if (x86_handle_page_fault(frame, cr2) == Status::Ok) {
                return false;
            }
            if (is_from_uspace(frame)) {
                *user_exception =
                    make_user_exception(task::ExceptionType::FatalPageFault, frame, cr2);
                return true;
            }
            panic_exception(frame, "Unhandled page fault");
        }

        // A fault a program raised is its own, not the kernel's: it is handed
        // up to kill the process rather than handled -- or panicked on --
        // below. The switch below is what ring 0 gets.
        if (is_from_uspace(frame)) {
            if (auto const type = user_exception_type(vector)) {
                *user_exception = make_user_exception(*type, frame, 0);
                return true;
            }
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
                // The lockup detector's, whoever sent it: its own report
                // asking this CPU for its state, or an NMI from outside the
                // machine asking for a report. An NMI cannot be masked, so it
                // can land while this very CPU holds the serial lock in the
                // middle of a log line; logging from here would then spin on
                // that lock forever, its holder waiting on the NMI to return.
                // So nothing on the NMI path takes a lock, and the detector
                // logs only once it has put the logger in panic mode, where a
                // write stops waiting for the lock after a bound.
                lockup_detector::on_nmi(*frame);
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
                // The root controller's driver ends the interrupt, one way or
                // the other: a line's flow ends its own through the line's
                // chip, and the handler ends one no line has.
                irq::handle_root_irq(frame);
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
                // Nothing arms the PMU to interrupt: its LVT entry stays masked
                // from init_apic_pmi() on, since the profiler samples from
                // timers and the PMU is to return as a facility of its own,
                // not as a sampling source. A PMI here is unexpected; say so
                // and acknowledge it rather than panic.
                log::warn("APIC PMI with no PMU user");
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

            case IrqVec::IpiTimer:
                x86_handle_ipi_timer();
                apic_issue_eoi();
                break;

            case IrqVec::IpiSuspend:
                x86_handle_ipi_suspend();
                // Never return.
                break;

            default:
                panic_exception(frame, "Unhandled interrupt vector");
        }
        return false;
    }

    /// Delivers a user exception the dispatcher returned, and answers what
    /// becomes of the thread: Resume, or End (the object layer has given it
    /// the Kill signal, which the caller's signal check acts on).
    ///
    /// Runs after the interrupt is fully put away (arch_handle_exception()),
    /// because delivery can block. Interrupts are on for the object layer,
    /// which takes sleeping locks, and off again when this returns, as the
    /// caller's signal check and the way back to ring 3 need them.
    [[nodiscard]]
    auto deliver_user_exception(arch::IrqFrame const *const frame,
                                task::ExceptionContext const &context) -> task::ExceptionOutcome {
        using task::Thread;

        // The frame is on this thread's own kernel stack, which is what lets
        // it outlive delivery, which can block and switch the thread out, and
        // what a resumed thread returns to ring 3 through. It stays true while
        // every exception uses the one kernel stack; a #DB moved onto an IST
        // stack (percpu.hpp reserves DEBUG_BREAK_STACK) would break it, and
        // this is where that would show.
        auto const &stack = Thread::Current::get()->kernel_stack();
        auto const where = reinterpret_cast<VirtAddr>(frame);
        DEBUG_ASSERT(where >= stack.base() && where < stack.top(),
                     "exception frame {:#x} is not on the thread's kernel stack", where);

        arch::AutoInterruptEnabler const irqs_on;
        return Thread::Current::on_user_exception(context);
    }

} // namespace

/// The registers of a user exception, after the line that reports it
/// (ThreadDispatcher::on_user_exception()). The same set panic_exception()
/// prints, at warning level: a program dying of its own fault is not a kernel
/// error, but whoever reads the log wants the registers all the same.
///
/// An exception raised from inside a system call has no frame, and the line
/// says so rather than printing nothing.
auto task::arch_dump_exception_context(task::ExceptionContext const &context) -> void {
    auto const *const frame = context.frame;
    if (frame == nullptr) {
        log::warn("  no frame to dump: raised from inside a system call");
        return;
    }
    log::warn("  rax={:#x}  rbx={:#x}  rcx={:#x}  rdx={:#x}", frame->ax, frame->bx, frame->cx,
              frame->dx);
    log::warn("  rsi={:#x}  rdi={:#x}  rbp={:#x}", frame->si, frame->di, frame->bp);
    log::warn("  r8={:#x}  r9={:#x}  r10={:#x}  r11={:#x}", frame->r8, frame->r9, frame->r10,
              frame->r11);
    log::warn("  r12={:#x}  r13={:#x}  r14={:#x}  r15={:#x}", frame->r12, frame->r13, frame->r14,
              frame->r15);
}

/// The pending-signal check on the way out of a system call, called by
/// x86_syscall (syscall.S) after the dispatcher returns and before the saved
/// user registers are popped. Interrupts are off, as process_pending_signals()
/// requires. It may not return: a pending Kill exits the thread here.
///
/// The saved user registers are not passed: nothing here reads them yet. A
/// debugger that reads a suspended thread's state will need them.
extern "C" auto x86_syscall_process_pending_signals() -> void {
    task::Thread::Current::process_pending_signals();
}

/// The C++ half of paranoid_common (exception.S): NMI, #DF and #MC, which
/// idt.S keeps away from interrupt_common and arch_handle_exception().
///
/// %gs first. These three can interrupt a swapgs pair half done, so on entry
/// %gs.base may be the user's even though the frame's CS says ring 0 (the
/// comment on paranoid_common has the windows). Until the ScopedInstall below
/// is built nothing may touch per-CPU storage -- no Cpu::id(), no
/// Thread::Current, no logging -- which is why the CPU comes from TR. It
/// holds because this kernel compiles with -fno-stack-protector and traps
/// UBSan in place: no instrumentation reads %gs behind the code's back. A
/// stack protector keyed off %gs, or a tracing hook on function entry, would
/// have to be kept out of this function and of the two it calls first.
///
/// Then none of arch_handle_exception()'s bookkeeping. These vectors arrive
/// anywhere -- inside another interrupt's handler, between the preamble and
/// postamble of one, with a per-CPU structure half updated -- and the
/// bookkeeping is exactly what they must not touch:
///
///  - the preamble and postamble write the per-CPU CURRENT_FRAME and move
///    the preempt count. An NMI nested in an IRQ used to run the full set, so
///    its postamble cleared CURRENT_FRAME to null under the IRQ still on the
///    stack, which then carried on believing it had a frame;
///  - preempting would switch threads while NMIs are still masked, which they
///    stay until the next iretq, and that iretq would then belong to some
///    other thread's return path;
///  - processing signals could exit the thread from inside an NMI.
///
/// #DF and #MC stop the kernel in x86_dispatch_exception() and never return;
/// they come this way because nothing on a double-fault or machine-check path
/// should depend on per-CPU state being consistent -- %gs included. None of
/// the three is a user exception (user_exception_type()), so nothing comes
/// back to deliver.
///
/// arch_handle_exception() asserts that none of the three ever reaches it.
extern "C" auto x86_handle_paranoid_exception(arch::IrqFrame *frame) -> void {
    mm::CpuLocal::ScopedInstall const kernel_gs{x86_cpu_num_from_tr()};

    task::ExceptionContext unused{};
    auto _ = x86_dispatch_exception(frame->vector, frame, &unused);
}

extern "C" auto arch_handle_exception(arch::IrqFrame *frame) -> void {
    using task::Thread;
    auto const vector = frame->vector;

    // NMI, #DF and #MC never come here: idt.S sends them to paranoid_common
    // and x86_handle_paranoid_exception(), which explains why.
    DEBUG_ASSERT(vector != arch::IrqVec::Nmi && vector != arch::IrqVec::DoubleFault &&
                     vector != arch::IrqVec::MachineCheck,
                 "vector {} belongs on the paranoid path", u32(vector));

    auto const from_user = is_from_uspace(frame);

    task::ExceptionContext user_exception{};
    auto *const previous_frame = irq::enter_trap(frame);
    auto const raised = x86_dispatch_exception(vector, frame, &user_exception);
    auto const need_preempt = irq::leave_trap(previous_frame);

    // The order is: finish the interrupt, then preempt, then signals. Signals
    // come last because the next change to them lets a pending Kill exit the
    // thread from here, and a thread that exits must not leave behind the
    // preempt count the preamble raised or a CURRENT_FRAME pointing into its
    // dead stack -- so the interrupt has to be fully put away first. They
    // also come after the preemption, so that a signal sent while this thread
    // was switched out is seen before it returns to ring 3 rather than one
    // interrupt later.
    if (need_preempt) {
        Thread::Current::preempt();
    }
    if (from_user) {
        log::trace("Exception[{}] from user space", to_string(vector));
        // Only a ring-3 exception is ever raised, so this is the one place
        // it can be delivered.
        auto const outcome = [&] {
            if (!raised) {
                return task::ExceptionOutcome::Resume;
            }
            return deliver_user_exception(frame, user_exception);
        }();

        // Resume returns to ring 3 through |frame| once this function is done;
        // End does not come back from here, because the Kill signal it stands
        // for makes this check exit the thread.
        Thread::Current::process_pending_signals();
        if (task::ExceptionOutcome::End == outcome) {
            panic("a user exception ({}) did not end thread {}",
                  task::to_string(user_exception.type), Thread::Current::get()->name());
        }
    }

    // Still off, as the stub's iretq path requires: nothing above may leave
    // interrupts enabled on its way back here.
    DEBUG_ASSERT(arch::irqs_disabled(), "interrupts enabled on the way out of exception {}",
                 to_string(vector));
}

} // namespace ours

/// One CPU's registers in the lockup detector's dump: the lines a kernel
/// fault's report prints. The detector calls it from an NMI, on the reporting
/// CPU only, once the logger is in panic mode.
auto lockup_detector::arch_dump_registers(arch::IrqFrame const &frame) -> void {
    ours::log_registers(frame);
}
