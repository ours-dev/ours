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

    /*
     * physload's own build ID, kept locatable at runtime rather than only
     * in the section-header table (which strip-sections drops from the
     * item physload actually ships as).  Assigned to both :rodata (so it is
     * physically loaded) and :note (so a PT_NOTE-walking tool finds it the
     * ordinary way too).  ModuleLoader compares a loaded module's PT_INTERP
     * against this before calling into it (ours/phys/build-id.hpp).
     */
    . = ALIGN(8);
    .note.gnu.build-id : {
        PROVIDE_HIDDEN(__build_id_note_start = .);
        KEEP(*(.note.gnu.build-id))
        PROVIDE_HIDDEN(__build_id_note_end = .);
    } :rodata :note

    /*
     * physload's module ABI.  gen-phys-exports.py emits one
     * {name, address} row per exported symbol; ModuleLoader scans this
     * range to bind a module's undefined symbols (ours/phys/exports.hpp).
     * KEEP: nothing references the rows, so --gc-sections would drop them.
     *
     * Explicit :rodata (not just inherited): the .note.gnu.build-id section
     * above closes with :rodata :note, and an untagged section inherits
     * whatever PHDR list came before it -- without this tag, .phys.exports
     * (and .ccrt_init after it) would silently fall into :note too,
     * stretching the PT_NOTE segment over sections that are not notes.
     */
    . = ALIGN(8);
    .phys.exports : {
        PROVIDE_HIDDEN(__phys_exports_start = .);
        KEEP(*(.phys.exports))
        PROVIDE_HIDDEN(__phys_exports_end = .);
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
        /* logz4::Domain registry; see kernel.ld. Placed explicitly rather
         * than left an orphan: an orphan section is put wherever the linker
         * judges closest by flags, and with PHDRS spelled out it would
         * inherit whichever segment precedes it. It holds pointers (the
         * domain names), so it must be in :data for the relative fixups to
         * land -- the same constraint as .phys.exports above. phys has no
         * console to look domains up from, but this image links the registry
         * code, which names the bounds. */
        PROVIDE_HIDDEN(__start_logz4_domains = .);
        KEEP(*(logz4_domains))
        PROVIDE_HIDDEN(__stop_logz4_domains = .);

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
