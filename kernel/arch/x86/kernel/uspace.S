#include <ours/arch/asm.hpp>
#include <arch/x86/macro/irqframe.hpp>

/// extern "C" auto x86_enter_uspace(arch::IrqFrame *t) -> void
///
/// Restoring user's status and changing privileges.
.function x86_enter_uspace, global, align=64, cfi=custom
    // push a fake 64bit interrupt stack frame and iret to it
    push_val X86_IRQFRAME_OFFSET_USS(%rdi) // ss
    push_val X86_IRQFRAME_OFFSET_USP(%rdi) // sp
    push_val X86_IRQFRAME_OFFSET_FLAGS(%rdi)   // rflags
    push_val X86_IRQFRAME_OFFSET_CS(%rdi)      // cs
    push_val X86_IRQFRAME_OFFSET_IP(%rdi)      // pc

    // Copy the remaining register state from |t|.
    movq X86_IRQFRAME_OFFSET_AX(%rdi), %rax
    movq X86_IRQFRAME_OFFSET_BX(%rdi), %rbx
    movq X86_IRQFRAME_OFFSET_CX(%rdi), %rcx
    movq X86_IRQFRAME_OFFSET_DX(%rdi), %rdx
    movq X86_IRQFRAME_OFFSET_BP(%rdi), %rbp
    movq X86_IRQFRAME_OFFSET_SI(%rdi), %rsi
    // Setting rdi deferred to last, we're still using it.
    movq X86_IRQFRAME_OFFSET_R8(%rdi), %r8
    movq X86_IRQFRAME_OFFSET_R9(%rdi), %r9
    movq X86_IRQFRAME_OFFSET_R10(%rdi), %r10
    movq X86_IRQFRAME_OFFSET_R11(%rdi), %r11
    movq X86_IRQFRAME_OFFSET_R12(%rdi), %r12
    movq X86_IRQFRAME_OFFSET_R13(%rdi), %r13
    movq X86_IRQFRAME_OFFSET_R14(%rdi), %r14
    movq X86_IRQFRAME_OFFSET_R15(%rdi), %r15
    movq X86_IRQFRAME_OFFSET_DI(%rdi), %rdi

    swapgs

    // iretq, not iret. Bare `iret` assembles to the 32-bit form (opcode CF,
    // `iretl`) even in 64-bit mode: it pops EIP, CS, EFLAGS, ESP and SS as
    // *four*-byte values, while everything pushed above is eight bytes wide.
    // The privilege change then took its return address, code selector and
    // flags out of the wrong halves of the frame and #GP'd, which triple
    // faulted because it happened before ring 3 was ever reached.
    // interrupt_common's return path spells it `iretq` for the same reason.
    iretq
.end_function