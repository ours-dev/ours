#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/arch/asm.hpp>

#include <ours/mm/cfg.hpp>

/// The symbols below should be defined in *.cpp files. It is because of
/// controlling the size of them for difference in 32-bits and 64-bits platform.
.extern KERNEL_VIRT_BASE
.extern KERNEL_PHYS_BASE
.extern KERNEL_STACK_BOTTOM

// Kernel entry point
.extern start_kernel

/// [[NO_RETURN]] 
/// auto _start(PhysAddr handoff) -> void
/// %rdi: relocated_base
.align 8
.function _start, global
    // Disable interrupts
    cli

    // Raw boot marker: one byte straight to COM1 before anything else, so a
    // boot that dies before the first kernel log line is still datable from
    // the serial output alone. THR accepts the byte unconditionally under
    // QEMU; losing it on real hardware costs nothing.
    mov $0x3F8, %dx
    mov $'K', %al
    out %al, %dx

    // Save handoff
    mov %rdi, %r15

    // Reload kernel stack and clear %rbp
    lea KERNEL_STACK_END(%rip), %rsp
    xor %rbp, %rbp

    mov %rsp, KERNEL_STACK_BOTTOM(%rip)
    mov %cr3, %rax
    mov %rax, KERNEL_PGD(%rip)

    mov $'S', %al
    out %al, %dx

    lgdt TEMP_GDTR(%rip)

    // Zero our kernel segment data registers
    xor %eax, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %fs
    mov %eax, %gs
    mov %eax, %ss

    mov $'G', %al
    out %al, %dx

    // Call our main module
    mov %r15, %rdi  // Pass handoff
    call start_kernel

idle:
    hlt
    pause
    jmp idle
.end_function

.object KERNEL_STACK, type=bss, scope=global, align=8
    .skip (PAGE_SIZE << 2) // 16K
.label KERNEL_STACK_END, global
.end_object

/// Export for main.ld
.label MAIN_ENTRY, global, value=_start