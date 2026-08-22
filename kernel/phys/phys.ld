ENTRY(PHYS_ENTRY)

PHDRS {
    code    PT_LOAD  FLAGS(5);  /* R|X */
    rodata  PT_LOAD  FLAGS(4);  /* R */
    data    PT_LOAD  FLAGS(6);  /* R|W */
    note    PT_NOTE  FLAGS(0);
}

SECTIONS {
    . = PHYS_LOAD_ADDRESS;
    PROVIDE_HIDDEN(__executable_start = .);
    . = ALIGN(8);
    .boot : {
        KEEP(*(.boot.header*))
        KEEP(*(.boot.text*))
    } :code
    
    . = ALIGN(8);
    .text : {
        *(.text*)
    }
    
    /*
     * PLT entry sections.
     *
     * The compiler and linker should never generate PLT entries since all
     * symbols have hidden visibility.
     */
    .plt : { *(.plt) }
    ASSERT(SIZEOF(.plt) == 0, "unexpected '.plt' entries")
    .plt.got : { *(.plt.got) }
    ASSERT(SIZEOF(.plt.got) == 0, "unexpected '.plt.got' entries")

    . = ALIGN(8);
    .rodata : {
        *(.rodata*)
    } :rodata

    . = ALIGN(8);
    .ccrt_init : {
        PROVIDE_HIDDEN(__ccrt_init_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__ccrt_init_end  = .);

        ASSERT(ALIGNOF(.ccrt_init) == 8 || SIZEOF(.ccrt_init) == 0, ".ccrt_init overalignment -> maybe padding gaps");
    }

    .data : {
        . = ALIGN(8);
        *(.data*)
    } :data

    . = ALIGN(8);
    .bss : {
        PROVIDE_HIDDEN(__bss_start = .);
        *(.sbss*) *(.bss* COMMON)
        PROVIDE_HIDDEN(__bss_end = .);
    }
    PROVIDE_HIDDEN(__executable_end = .);
}
