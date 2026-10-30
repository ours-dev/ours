#include <ours/arch/x86/asm.hpp>
#include <ours/mm/cfg.hpp>

/// Common to 32/64 bits platform.
.object TEMP_GDTR, type=data, scope=global, align=8
    .short EARLY_GDT_END - EARLY_GDT - 1
    .quad EARLY_GDT
.end_object

/// GDT
.object EARLY_GDT, type=data, scope=global, align=8
    .quad 0                   # NULL
    .quad 0x00cf9a000000ffff  # kernel code32 (0x08)
    .quad 0x00af9a000000ffff  # kernel code64 (0x10)
    .quad 0x00cf92000000ffff  # kernel data (0x18)
.label EARLY_GDT_END, global
.end_object

.object TEMP_IDTR, type=data, scope=global, align=8
	.word EARLY_IDT_END - EARLY_IDT - 1
	.quad 0
.end_object

	.balign 8
.object EARLY_IDT, type=data, scope=global, align=8
	.rept EALY_IDT_ENTRIES
	.quad 0
	.quad 0
	.endr
.label EARLY_IDT_END, global
.end_object

/// Boot stack. physload, physboot and the kernel all run as nested calls on
/// this one stack (nothing downstream switches stacks before the kernel
/// does), and the module loader's BootFS materialization is the deepest
/// consumer; 16K overflowed into the objects below (EARLY_IDT first), which
/// surfaced as corrupted globals and silent triple faults mid-boot.
.object EARLY_STACK, type=bss, scope=global, align=8
    .skip (PAGE_SIZE << 6) // 256K
.label EARLY_STACK_END, global
.end_object

.section .note.GNU-stack,"",@progbits
