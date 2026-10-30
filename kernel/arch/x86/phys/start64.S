#include <ours/arch/x86/asm.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/arch/x86/descriptor.hpp>

#include <arch/x86/macro/mmu.hpp>
#include <arch/x86/macro/msr.hpp>
#include <arch/x86/macro/system.hpp>

// Supposing _start64 is the head of phys, nameyly the first symbol.
#define rva(symbol) (symbol - _start64)

.extern TEMP_GDTR
.extern EARLY_STACK_END

.section ".boot.text", "a", @progbits

/// [[no_return]] 
/// auto _start64(Address %edi, LegacyBootParam %esi) -> void
.function _start64, scope=global
.label x86_start, global
.code32
    cli

    // Do not modify, it is coordinate with rva(symbol) to provide
    // a relative addressing ability in 32 bit platform.
    mov %edi, %ebx

    // Clear any incoming stack pointer so it can't be used accidentally
    // before 
    // the proper stack is set up below.
    xor %ebp, %ebp

    // Clear BSS
    cld
    lea rva(__bss_start)(%ebx), %edi
    lea rva(__bss_end)(%ebx), %ecx
    sub %edi, %ecx
    xor %eax, %eax
    shr $2, %ecx
    rep stosl // Repeat eax -> ds:edi

    // Load ealry stack
    lea rva(EARLY_STACK_END)(%ebx), %esp

    // Scratch argument constructed to dummy 8 bytes
    push $0
    push %esi

    lgdtl rva(TEMP_GDTR)(%ebx)

    mov $X86_GDT_KERNEL_DATA, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %fs
    mov %eax, %gs
    mov %eax, %ss

    pushl $X86_GDT_KERNEL_CODE32
    lea rva(0f)(%ebx), %eax
    push %eax
    lretl
0:
    // CPUID will clobbers EBX, so we must save it shortly.
    push %ebx

    // Check if the system supports long mode.
    mov $0x80000000, %eax   // Set the A-register to 0x80000000.
    cpuid                   // CPU identification.
    cmp $0x80000001, %eax   // Compare the A-register with 0x80000001.
    jb .Lunsupported_cpu    // It is less, there is no long mode.

    mov $0x80000001, %eax   // Set the A-register to 0x80000001.
    cpuid                   // CPU identification.
    // Long mode (EDX[29]) and NX (EDX[20]) are both hard requirements. NX is
    // one because physboot marks every non-executable kernel mapping with the
    // NX bit: without EFER.NXE that bit is reserved, so each such page would
    // fault on its first touch rather than merely lose its protection.
    mov $((1 << 29) | (1 << 20)), %eax
    and %eax, %edx
    cmp %eax, %edx
    je 0f

.Lunsupported_cpu:
    hlt
    pause
    loop .Lunsupported_cpu

    // We support long mode.
0:
    pop %ebx

    // Disable paging
    mov %cr0, %eax
    and $(~X86_CR0_PG), %eax
    mov %eax, %cr0

    // Build leaf entries for page table
    // %eax: Pfn; 
    // %ecx: Number of page to map; 
    // %edx: Temporary variable for constructing PTE
    // %edi: Pointer pointed to PTE 
    lea rva(PD)(%ebx), %edi
    movl $(X86_MAX_PTES << 2),  %ecx
    // here %eax is the PFN part of a large page frame.
    xor  %eax, %eax
1:
    // Handle low 32 bits
    mov  %eax, %edx
    shll $21,  %edx 
    orl  $X86_KERNEL_HUGE_PAGE_FLAGS, %edx
    movl %edx, (%edi)
    // Handle high 32 bits
    mov  %eax, %edx
    shrl $11,  %edx     // Skip the remaining 11 bits. 
    movl %edx, 4(%edi)  // Save the high 32 bits of address.
    // Update for next iteration.
    addl $8, %edi
    inc  %eax
    loop 1b

    // PDP[0] = PD | X86_KERNEL_PD_FLAGS
    lea rva(PD)(%ebx), %eax
    or $X86_KERNEL_PD_FLAGS, %eax
    mov %eax, rva(PDP)(%ebx)

    // PML4[0] = PDP | X86_KERNEL_PD_FLAGS
    lea rva(PDP)(%ebx), %eax
    or $X86_KERNEL_PD_FLAGS, %eax
    mov %eax, rva(PML4)(%ebx)

    // Start installation.

    // Disable global paging.
    mov   %cr4, %eax
    and   $(~X86_CR4_PGE), %eax
    mov   %eax, %cr4

    // Load the physical pointer to the root page table.
    lea  rva(PML4)(%ebx), %eax
    mov  %eax, %cr3

    // Now the new root has been installed, re-enable global pages and set PAE.
    mov  %cr4, %eax
    or   $(X86_CR4_PGE | X86_CR4_PAE), %eax
    mov  %eax, %cr4

    // Enable long mode, and NX along with it -- the kernel page tables
    // physboot builds set the NX bit, which is reserved while NXE is clear.
    mov $X86_MSR_IA32_EFER, %ecx
    rdmsr
    or  $(X86_EFER_LME | X86_EFER_NXE), %eax
    wrmsr

    // We are still in 32bit compability mode, but just through loading 64-bit CS
    // we can enter the real 64-bit mode.
    lgdtl rva(TEMP_GDTR)(%ebx)
    lea rva(startup64)(%ebx), %eax

    pushl $X86_GDT_KERNEL_CODE64
    push %eax

    // Enable paging, with supervisor write protection on from the start:
    // without CR0.WP a read-only mapping stops nothing in ring 0, so the
    // kernel's own read-only segments would only become enforced once
    // x86_init_mmu_percpu() runs, long after it has started executing.
    mov %cr0, %eax
    or  $(X86_CR0_PG | X86_CR0_WP), %eax
    mov %eax, %cr0

    lretl

.code64
.align 8
startup64:
    cld
    cli

    // We got here.
	xorl	%eax, %eax
	movl	%eax, %ds
	movl	%eax, %es
	movl	%eax, %ss
	movl	%eax, %fs
	movl	%eax, %gs

    xor    %rax, %rax
    xor    %rbx, %rbx
    xor    %rcx, %rcx
    xor    %rdx, %rdx
    xor    %rbp, %rbp
    xor    %rsi, %rsi

    // Restore argument from stack
    pop %rdi
    call phys_main

.end_function

    // Never reach at here.
0:
    hlt
    pause
    loop 0b

// Page Table
.object PML4, type=bss, align=PAGE_SIZE
.label PGD, global
    .skip PAGE_SIZE
.end_object

.object PDP, type=bss, align=PAGE_SIZE
    .skip PAGE_SIZE
.end_object

.object PD, type=bss, align=PAGE_SIZE
    .skip (PAGE_SIZE << 2)
.end_object

.section .note.GNU-stack,"",@progbits
