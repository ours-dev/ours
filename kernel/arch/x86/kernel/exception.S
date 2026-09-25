#include <ours/arch/asm.hpp>

#include <arch/x86/macro/irqframe.hpp>

// This is the real function all the macro-generated functions tail-call into.
.function interrupt_common, global, align=64, cfi=custom
    // Set CFI for an interrupt frame, with all the words pushed by the hardware
    // and the macro-generated functions together.
    .cfi_signal_frame
    .cfi_def_cfa %rsp, 7 * 8
    .cfi_offset %rip, -(5 * 8)

    // Mark each reg as having the same value as from the "calling" frame.
    // This is the default state for callee-saved registers, but for completeness
    // sake we do this for all of them.
    all_cfi_same_value

    // Clear the direction flag.  Without this, uses of string
    // instructions, e.g. REP MOVS in memcpy() or inlined by the compiler,
    // can go wrong and copy in the wrong direction, since this code may
    // assume that the direction flag is unset.
    cld

    // At this point the stack contains 7 qwords.  5 or 6 pushed by the interrupt
    // and 2 or 1 pushed by the stub.
    save_general_registers

    // Zero general-purpose registers to constrain speculative execution with
    // potentially-hostile register values; the values may have come from user
    // mode and could be consumed by in-kernel speculative execution.
    // Zeroing of ebp is delayed till we check precisely if this was a user fault
    // so that kernel faults can have correct stack frames for backtraces.
    zero_general_registers

    // At this point the stack contains a complete Iframe.

    // Pass the IrqFrame in %rdi.
    mov %rsp, %rdi

    // Check to see if we came from user space by testing the CPL in the
    // %cs selector that was saved on the stack automatically.  Check for != 0.
    testb $3, X86_IRQFRAME_OFFSET_CS(%rsp)
    jz 1f

    // Perform the last zero from the previous block now that we know this is a
    // user fault and we do not need the stack frame.
    xorl %ebp, %ebp
    // Swap %gs.base to kernel space.
    swapgs
1:
    // Mitigates the swapgs bug. See <arch/code-patches/case-id.h>.
    lfence

    call arch_handle_exception

    // Check if we're returning to user space as per before.
    testb $3, X86_IRQFRAME_OFFSET_CS(%rsp)
    jz    1f

    // Swap gs_base back to user space.
    swapgs
1:
    lfence

.Lcommon_return:
    // Restore general purpose registers.
    restore_general_registers

    // Drop vector number and error code.
    add_to_sp 16

    iretq

.end_function

// The entry for NMI, #DF and #MC; idt.S routes those three here and every
// other vector to interrupt_common.
//
// What sets it apart is the missing swapgs. interrupt_common swaps %gs when
// the saved CS says the CPU came from ring 3, which is right for anything
// that cannot fire while a swapgs pair is half done. These three can: an NMI
// or #MC lands between SYSCALL and its swapgs, between the exit swapgs and
// SYSRET, or in the same spots around every other interrupt taken from ring
// 3 -- CS already ring 0, %gs.base still the user's. Testing CS there would
// leave the user's %gs in place, and with FSGSBASE a user can make that any
// value it likes. So nothing is decided here: x86_handle_paranoid_exception()
// works out which CPU it is on from TR, installs that CPU's %gs, and puts the
// old value back before returning (mm::CpuLocal::ScopedInstall). Whatever the
// interrupted code was in the middle of, it resumes with the %gs it had.
//
// No lfence either. The one in interrupt_common guards the conditional
// swapgs against speculation (the "swapgs bug"); there is no conditional
// swapgs here, and the wrmsr that does change %gs is serialising.
//
// Zircon counterpart: the .Lnmi branch of interrupt_common, which brackets
// x86_nmi_handler() with rdmsr/wrmsr of MSR_IA32_GS_BASE. Linux:
// paranoid_entry / paranoid_exit in arch/x86/entry/entry_64.S.
.function paranoid_common, global, align=64, cfi=custom
    .cfi_signal_frame
    .cfi_def_cfa %rsp, 7 * 8
    .cfi_offset %rip, -(5 * 8)
    all_cfi_same_value

    cld

    save_general_registers
    zero_general_registers

    // Pass the IrqFrame in %rdi.
    mov %rsp, %rdi
    call x86_handle_paranoid_exception

    restore_general_registers

    // Drop vector number and error code.
    add_to_sp 16

    iretq

.end_function
