#include <ours/arch/asm.hpp>
#include <ours/arch/x86/bootstrap.hpp>
#include <ours/arch/x86/descriptor.hpp>
#include <ours/mm/cfg.hpp>

#include <arch/x86/macro/system.hpp>
#include <arch/x86/macro/msr.hpp>

/// Why do we need this?
///
/// Since under real mode, we are limited to just access the memory space lower than
/// 1 MB, but our main module which include the following trampoline code was loaded 
/// at address which is over N GB. So that The following code must be copied to low 
/// address, usually first k pages, therefore a new problem related with relocation
/// was stemd from that. 
///
/// To sidestep the relocation, rva() names a symbol by its offset from
/// __rva_start, so the code addresses its own data relative to wherever it
/// was copied.
///
#define rva(symbol) (symbol - __rva_start)

.section ".init.boot.head", "ax", %progbits
.balign PAGE_SIZE
.label __x86_bootstrap16_start, global

.code16
.label x86_start_bootstrap16, global
    cli
    cld

    // Non-cache mode.
    mov %cr0, %eax
    or $X86_CR0_CD, %eax
    and $~X86_CR0_NW, %eax
    mov %eax, %cr0

    mov %cs, %si
    // Let ds and ss use the 2nd page.
    add $0x100, %si
    mov %si, %ds
    mov %si, %ss

    lgdtl X86_BOOTSTRAP_OFFSET_GDTR

    // Enter protected mode without paging
    mov %cr0, %eax
    or $X86_CR0_PE, %eax
    mov %eax, %cr0

    // Enable PAE and PGE 
    mov %cr4, %eax
    or $(X86_CR4_PAE | X86_CR4_PGE), %eax
    mov %eax, %cr4

    // Load CR3 with the PGD (It's address is in 32bits)
    mov X86_BOOTSTRAP_OFFSET_PGD, %eax
    mov %eax, %cr3

    // Enable long mode
    mov $X86_MSR_IA32_EFER, %ecx
    rdmsr
    or $X86_EFER_LME, %eax
    or $X86_EFER_NXE, %eax
    wrmsr

    // enable paging
    mov %cr0, %eax
    or $X86_CR0_PG, %eax
    mov %eax, %cr0

//     // Translate data page segment into full address
    mov %ds, %ebx
    shl $4, %ebx

    // Reload CS and jump to 64-bit code.
    mov $X86_BOOTSTRAP_OFFSET_LONGMODE_PHYS_ENTRY, %esp // ss:X86_BOOTSTRAP_OFFSET_LONGMODE_PHYS_ENTRY
    lretl

.code64
.label x86_ap_enter_long_mode, global 
    // Zero our data segments
    xor %eax, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %fs
    mov %eax, %gs
    mov %eax, %ss

    // The far entry is x86_start_nonboot_cpu(CpuNum cpunum, Thread *thread),
    // so the System V argument registers have to be filled in that order:
    // %rdi is the CPU number and %rsi the thread. They were assigned the
    // other way round -- %rsi the number, %rdi the thread -- which handed
    // CpuLocal::init_percpu() the low 32 bits of a kernel pointer as a CPU
    // number and stopped the first AP on its `cpu < MAX_CPU` assertion.
    //
    // `movl` into %edi is enough: a 32-bit destination is zero-extended to
    // the full register in 64-bit mode, so no `xor` is needed first.
    movl X86_BOOTSTRAP_OFFSET_CPUNUM(%rbx), %edi

.label __debug_symbol0, global 
    // Let %rsp point to kernel stack of current thread.
    mov X86_BOOTSTRAP_OFFSET_STACK_TOP(%rbx), %rsp

.label __debug_symbol1, global 
.label __debug_symbol2, global
    // Let %rsi point to current thread
    mov X86_BOOTSTRAP_OFFSET_THREAD(%rbx), %rsi

.label __debug_symbol3, global
    // Two things the callee is entitled to assume and that a bare `jmp` into
    // it would not give:
    //
    //   - A terminated frame-pointer chain. %rbp is whatever the firmware
    //     left behind, so an unwinder walking out of the idle loop would
    //     follow it into nothing.
    //   - The stack alignment the SysV ABI states: %rsp is 16-byte aligned
    //     at a `call`, hence %rsp % 16 == 8 once the return address is on
    //     the stack, and that is the offset the callee's prologue is
    //     compiled against. A stack top is page aligned, so jumping straight
    //     in would leave %rsp % 16 == 0 and every 16-byte spill the compiler
    //     emits (`movaps`) would #GP.
    //
    // Pushing a null return address fixes both at once and is what `call`
    // would have done anyway; the callee is [[noreturn]], so it is never
    // popped.
    xor %ebp, %ebp
    pushq $0
.label __debug_symbol4, global
    jmp *X86_BOOTSTRAP_OFFSET_LONGMODE_VIRT_ENTRY(%rbx)
0:
    hlt
    jmp 0b

.label __x86_bootstrap16_end, global 