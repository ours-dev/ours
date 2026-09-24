#include <arch/x86/macro/syscallframe.hpp>
#include <ours/arch/asm.hpp>
#include <ours/arch/x86/percpu.hpp>

/// Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 2B:
///
/// SYSCALL saves the RIP of the instruction following SYSCALL to RCX and loads a new
/// RIP from the IA32_LSTAR (64-bit mode). Upon return, SYSRET copies the value
/// saved in RCX to the RIP. 
///
/// SYSCALL saves RFLAGS (lower 32 bit only) in R11. It then masks RFLAGS with an
/// OS-defined value using the IA32_FMASK (MSR C000_0084). The actual mask value
/// used by the OS is the complement of the value written to the IA32_FMASK MSR.
/// None of the bits in RFLAGS are automatically cleared (except for RF). SYSRET
/// restores RFLAGS from R11 (the lower 32 bits only).
///
/// Software should not alter the CS or SS descriptors in a manner that violates the
/// following assumptions made by SYSCALL/SYSRET:
///     1. The CS and SS base and limit remain the same for all processes, including the
///     operating system (the base is 0H and the limit is 0FFFFFFFFH).
///     2. The CS of the SYSCALL target has a privilege level of 0.
///     3. The CS of the SYSRET target has a privilege level of 3.
///
/// SYSCALL/SYSRET do not check for violations of these assumptions.
/// IF (CS.L ≠ 1 ) or (IA32_EFER.LMA ≠ 1) or (IA32_EFER.SCE ≠ 1) 
/// Namely Not in 64-Bit Mode or SYSCALL/SYSRET not enabled in IA32_EFER *)
///     THEN #UD; 
/// FI;
/// RCX ← RIP;
/// RIP ← LSTAR_MSR;
/// R11 ← EFLAGS;
/// EFLAGS ← (EFLAGS MASKED BY IA32_FMASK);
/// CPL ← 0;
/// CS(SEL) ← IA32_STAR_MSR[47:32];
/// CS(DPL) ← 0;
/// CS(BASE) ← 0;
/// CS(LIMIT) ← 0xFFFFF;
/// CS(GRANULAR) ← 1;
/// SS(SEL) ← IA32_STAR_MSR[47:32] + 8;
/// SS(DPL) ← 0;
/// SS(BASE) ← 0;
/// SS(LIMIT) ← 0xFFFFF;
/// SS(GRANULAR) ← 1;
///
/// On syscall,
///     CS = IA32_STAR_MSR[47:32]
///     SS = IA32_STAR_MSR[47:32] + 8

/// SYSCALL saves the RIP of the instruction following the SYSCALL into RCX and loads
/// the new RIP from the LSTAR (64-bit mode only). Upon return, SYSRET copies the
/// value saved in RCX to the RIP.
/// In a return to 64-bit mode using Osize 64, SYSRET sets the CS selector value to MSR
/// IA32_STAR[63:48] +16. The SS is set to IA32_STAR[63:48] + 8.
/// SYSRET transfer control to compatibility mode using Osize 32. The CS selector value
/// is set to MSR IA32_STAR[63:48]. The SS is set to IA32_STAR[63:48] + 8.
/// It is the responsibility of the OS to keep descriptors in the GDT/LDT that correspond
/// to selectors loaded by SYSCALL/SYSRET consistent with the base, limit and attribute
/// values forced by the these instructions.
/// Software should not alter the CS or SS descriptors in a manner that violates the
/// following assumptions made by SYSCALL/SYSRET:
/// • CS and SS base and limit remain the same for all processes, including the
/// operating system.
/// • CS of the SYSCALL target has a privilege level of 0.
/// • CS of the SYSRET target has a privilege level of 3.
/// SYSCALL/SYSRET do not check for violations of these assumptions.
/// IF (CS.L ≠ 1 ) or (IA32_EFER.LMA ≠ 1) or (IA32_EFER.SCE ≠ 1)
/// (* Not in 64-Bit Mode or SYSCALL/SYSRET not enabled in IA32_EFER *)
///     THEN #UD; 
/// FI;
/// IF (CPL ≠ 0)
///     THEN #GP(0); 
/// FI;
/// IF (RCX ≠ CANONICAL_ADDRESS)
///     THEN #GP(0);
/// FI;
/// IF (OPERAND_SIZE = 64)
///     THEN (* Return to 64-Bit Mode *)
///     EFLAGS ← R11;
///     CPL ← 0x3;
///     CS(SEL) ← IA32_STAR[63:48] + 16;
///     CS(PL) ← 0x3;
///     SS(SEL) ← IA32_STAR[63:48] + 8;
///     SS(PL) ← 0x3;
///     RIP ← RCX;
/// ELSE (* Return to Compatibility Mode *)
///     EFLAGS ← R11;
///     CPL ← 0x3;
///     CS(SEL) ← IA32_STAR[63:48];
///     CS(PL) ← 0x3;
///     SS(SEL) ← IA32_STAR[63:48] + 8;
///     SS(PL) ← 0x3;
///     EIP ← ECX;
/// FI;
///
/// On sysexit,
///     CS = IA32_STAR_MSR[63:48] + 16
///     SS = IA32_STAR_MSR[63:48] + 8


// kernel side of the SYSCALL instruction
// state on entry:
// RCX holds user RIP
// R11 holds user RFLAGS
// RSP still holds user stack
// CS loaded with kernel CS from IA32_STAR
// SS loaded with kernel CS + 8 from IA32_STAR

// Arguments passed:
//  rax - syscall # and return (bounds-checked in syscall_dispatch, not here)
//  rbx - saved
//  rcx - modified as part of syscall instruction
//  rdx - arg 3
//  rsi - arg 2
//  rdi - arg 1
//  rbp - saved
//  rsp - the *user* stack; saved into the frame (SyscallFrame::sp), passing
//        through this CPU's percpu block (X86_PERCPU_SAVED_USER_SP_OFFSET)
//        only for the two instructions it takes to reach the kernel stack
//  r8  - arg 5
//  r9  - arg 6
//  r10 - arg 4
//  r11 - modified as part of syscall instruction
//  r12 - arg 7
//  r13 - arg 8/  r14 - saved
//  r15 - saved
//
.function x86_syscall, global, align=64, cfi=custom
    // Swap to the kernel GS register
    swapgs

    // Swap stack.  The per-CPU slot is scratch, not storage: the user %rsp
    // goes from there into the frame at once, as its first (highest) member.
    // Leaving it in the slot for the whole call -- which this stub used to --
    // is correct only while no handler sleeps.  One that blocks lets another
    // thread's syscall on this CPU overwrite the slot, and a thread that wakes
    // on another CPU would read that CPU's slot; SYSRET then lands on a stack
    // that is not the caller's.  Linux's entry_SYSCALL_64 does the same dance
    // with TSS.sp2 and pt_regs->sp.
    mov %rsp, CPU_LOCAL_ACCESS(X86_PCPU + X86_PERCPU_SAVED_USER_SP_OFFSET)
    mov CPU_LOCAL_ACCESS(X86_PCPU + X86_PERCPU_SAVED_KERNEL_SP_OFFSET), %rsp
    // A bare pushq rather than push_val: the macro splits its operand on
    // whitespace, and CPU_LOCAL_ACCESS expands to a %gs-relative operand that
    // contains some.
    pushq CPU_LOCAL_ACCESS(X86_PCPU + X86_PERCPU_SAVED_USER_SP_OFFSET)  // SP

    // Everything below builds arch::SyscallFrame (arch/x86/syscall.hpp) on the
    // kernel stack, from its last member downwards -- the stack grows down, so
    // the first push lands at the highest offset.  The offsets both sides agree
    // on are in arch/x86/macro/syscallframe.hpp, and the struct static_asserts
    // itself against them; adding or reordering a push here without amending
    // that header is a build failure rather than a wrong register read.
    push_val %r11  // FLAGS: the user RFLAGS, which SYSCALL parked in %r11.
    push_val %rcx  // IP: the user return address, which SYSCALL parked in %rcx.
    push_val %r15
    push_val %r14
    push_val %r13
    push_val %r12
    push_val $0    // R11 was trashed by the syscall instruction.
    push_val %r10
    push_val %r9
    push_val %r8
    push_val %rbp
    push_val %rdi
    push_val %rsi
    push_val %rdx
    push_val $0    // RCX was trashed by the syscall instruction.
    push_val %rbx
    push_val %rax

    // The frame pointer is %rsp, which is where the last push left it -- not
    // %rbp.  %rbp holds a user-controlled value that this stub only ever
    // pushed; reading the frame through it handed every syscall handler a
    // pointer userspace chose, which is a kernel write primitive as soon as a
    // handler answers by storing into frame->ax.  interrupt_common in
    // exception.S has always passed %rsp here, for the same reason.
    mov %rsp, %rdi

    // One call, into ours::syscall_dispatch (kernel/lib/syscall/table.cpp),
    // rather than indexing SYSCALL_TABLE from here.
    //
    // The bounds clamp on %rax used to live here as a `cmov`, and it moved
    // there for a reason the assembly could not solve on its own: the kernel
    // now also has to refuse a trap that did not come from the calling
    // process's vDSO, and answering that means reaching the current thread --
    // whose pointer lives behind a mangled C++ per-CPU template that assembly
    // cannot name. Two checks in two languages guarding one door read worse
    // than both in the one place a new syscall cannot avoid.
    //
    // Reached %rip-relative: the kernel is a static PIE, and an absolute
    // displacement would emit R_X86_64_32S and break the link.
    call syscall_dispatch

    // Pending thread signals, last, with interrupts already off (the
    // dispatcher turns them off before it returns). A Kill raised while the
    // call ran -- by another thread, or by the call itself -- has to be
    // acted on before the thread is back in ring 3, not whenever it next
    // happens to take an interrupt; and with interrupts off, one raised after
    // this check is caught by the IPI its sender follows it with, rather than
    // lost between the check and SYSRET. %rsp is where the dispatcher found
    // it, still 16-byte aligned for the call.
    //
    // Zircon's syscall.S calls x86_syscall_process_pending_signals() at the
    // same point, but only when do_syscall()'s return says a signal is
    // pending. Here the call is unconditional: with nothing pending it is one
    // load and a return, and it saves threading a flag back through the
    // dispatcher.
    call x86_syscall_process_pending_signals

    pop_reg %rax
    pop_reg %rbx
    pop_reg %rcx  // Will be overwritten with RIP later on.
    pop_reg %rdx
    pop_reg %rsi
    pop_reg %rdi
    pop_reg %rbp
    pop_reg %r8
    pop_reg %r9
    pop_reg %r10
    pop_reg %r11  // Will be overwritten with RFLAGS later on.
    pop_reg %r12
    pop_reg %r13
    pop_reg %r14
    pop_reg %r15
    pop_reg %rcx  // RIP
    pop_reg %r11  // RFLAGS

    // Restore the user stack from the frame -- the slot this thread pushed,
    // on whichever CPU it happens to be leaving from.  `popq %rsp` loads the
    // popped value into %rsp outright (the increment is discarded), and it is
    // the last thing the kernel stack is used for; Linux ends its fast path
    // the same way.
    pop_reg %rsp

    // Swap GS
    swapgs

    // sysretQ.  Bare `sysret` assembles to the 32-bit form, and the operand
    // size is what selects which world the CPU returns to: with Osize 32 it
    // loads CS from IA32_STAR[63:48] itself -- X86_GDT_USER_CODE32 -- and only
    // %ecx into %eip, dropping the caller into compatibility mode to execute
    // its 64-bit code as 32-bit.  Osize 64 is the case the manual quoted at
    // the top of this file describes: CS = IA32_STAR[63:48] + 16, which is
    // X86_GDT_USER_CODE64, and the full %rcx into %rip.
    //
    // The first system call this kernel ever made printed its string and then
    // died on the way home for exactly this reason.
    sysretq

.end_function
