#include <ours/arch/asm.hpp>

/// NO_MANGLE auto x86_switch_context(VirtAddr *prev, VirtAddr next) -> void
/// %rdi: prev
/// %rsi: next
.function x86_switch_context, global, align=64, cfi=custom
    // Save callee-saved registers
    push_reg %rbx
    push_reg %rbp
    push_reg %r12
    push_reg %r13
    push_reg %r14
    push_reg %r15

    // Switch kernel stack from `prev's` to `next's`
    mov %rsp, %rdi
    mov %rsi, %rsp

    // Restore Callee-saved registers
    pop_reg %r15
    pop_reg %r14
    pop_reg %r13
    pop_reg %r12
    pop_reg %rbp
    pop_reg %rbx

    // Return to cpp-code
    ret
    int3
.end_function