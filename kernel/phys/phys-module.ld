/* Linker script for physload-loaded modules (physload_module() targets).
 *
 * The module is an ET_DYN image linked at base 0: physload places it
 * anywhere in memory and applies its relative relocations.  e_entry is the
 * standard adapter phys_load_handoff (physload-module.cpp), so the loader
 * needs no symbol lookup — it tail-calls e_entry.
 */

ENTRY(phys_load_handoff)

SECTIONS {
    . = 0;

    .text : {
        *(.text .text.*)
    }

    /*
     * Modules are compiled -fno-plt, so their own calls reach imports
     * through the GOT.  Objects shared with physload (ccrt, libc) are not,
     * and lld gives those calls PLT stubs.  ModuleLoader binds the
     * R_*_JUMP_SLOT relocations eagerly -- with -z now nothing ever tries
     * to resolve a stub lazily, which is what a boot image could not
     * service.
     */
    .plt : { *(.plt) }
    .plt.got : { *(.plt.got) }

    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(8);
    .ccrt_init : {
        PROVIDE_HIDDEN(__ccrt_init_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__ccrt_init_end = .);

        ASSERT(ALIGNOF(.ccrt_init) == 8 || SIZEOF(.ccrt_init) == 0,
               ".ccrt_init overalignment -> maybe padding gaps")
    }

    .data : {
        *(.data .data.*)
    }

    . = ALIGN(8);
    .bss : {
        PROVIDE_HIDDEN(__bss_start = .);
        *(.sbss* .bss* COMMON)
        PROVIDE_HIDDEN(__bss_end = .);
    }

    /DISCARD/ : {
        *(.note.GNU-stack)
    }
}
