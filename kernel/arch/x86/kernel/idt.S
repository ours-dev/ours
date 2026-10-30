#include <ours/arch/x86/asm.hpp>
#include <ours/mm/cfg.hpp>

.extern interrupt_common
.extern paranoid_common

// This is called 256 times in a row, with isr.current set to the iteration
// count starting at 0.  This is inside the definition for the ISR table,
// below (equivalent to being inside an .object ... .end_object pair,
// though those macros are not actually used because they don't allow
// nesting .function ... .end_function inside).  So the ambient section
// state is building up the table, and the macro defines a function using
// pushsection/popsection before adding its pointer to the table.
.macro isr.dispatch.define name
    // Make the whole set of macro-generated functions be cache-line aligned
    // collectively.
    .pushsection .text.isr.table, "ax", %progbits
    .ifeq isr.current
        .balign 64
    .endif
    .function \name, cfi=custom, align=8, nosection=nosection
        // Set CFI for an interrupt frame.
        .cfi_signal_frame
        isr.current.has_error = isr.current == 8 || \
                                (isr.current >= 10 && isr.current <= 14) || \
                                isr.current == 17

        .cfi_def_cfa %rsp, (8 * (5 + isr.current.has_error))
        .cfi_offset %rip, -(5 * 8)
        // Mark each reg as having the same value as from the "calling" frame.
        // This is the default state for callee-saved registers, but for
        // completeness sake we do this for all of them.
        all_cfi_same_value

        // Clear the AC flag to prevent ring 0 from performing data accesses to
        // ring 3 if SMAP is available.  If it was set, it will get restored by
        // iretq.  DO NOT REMOVE THIS CLAC, code in idt.cpp assumes it is here.
        // It MUST be the first instruction of this function.
        clac
     .if !isr.current.has_error
        // Fill in the error code not pushed by the hardware.
        push_val $0
     .endif
        // Fill in the interrupt number.
        push_val $isr.current

        // NMI (2), #DF (8) and #MC (18) go to paranoid_common, which does not
        // decide %gs from the saved CS: these three can land inside a swapgs
        // pair, where CS says ring 0 and %gs.base is still the user's. They
        // are also exactly the vectors given IST stacks (idt.cpp), and for the
        // same reason -- nothing about the interrupted context can be assumed.
        // Sorted here, when the table is assembled, so the other 253 stubs pay
        // nothing for it at run time.
        isr.current.is_paranoid = isr.current == 2 || isr.current == 8 || \
                                  isr.current == 18
     .if isr.current.is_paranoid
        jmp paranoid_common
     .else
        jmp interrupt_common
     .endif

        // Should never go back here.
        int3
    .end_function
    .popsection

    // Now we're back in the section building up the table.
    .quad \name
    .quad 0
.endm

.macro isr.dispatch.define.next
    // The name is generated with an arbitrary number and is not actually
    // meaningful at all.  But there's no way to get isr.current into a
    // symbol name here.
    isr.dispatch.define isr.dispatch.\@.is.not.the.isr.number
    isr.current = isr.current + 1
.endm

.section .data.rel.ro.idt, "aw", %progbits
.balign PAGE_SIZE
.label IDT, global, object
    isr.current = 0
    .rept 256
        isr.dispatch.define.next
    .endr
.size IDT, . - IDT