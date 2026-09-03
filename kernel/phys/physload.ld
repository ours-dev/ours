/* Linker script for physload itself.
 *
 * physload is loaded and relocated by boot-shim (ElfImage::load_at() +
 * relocate()) the same way physload loads physboot: an ET_DYN image linked
 * at base 0, placed wherever boot-shim finds room, with only relative
 * ("simple-fixup") relocations -- no symbol binding, since nothing exists
 * yet for physload to bind against.  ENTRY(obi_main) is the address
 * boot-shim tail-calls once loading and relocation finish (obi_main is
 * NO_MANGLE specifically so this line can name it).
 *
 * Everything Multiboot2-specific -- the .boot header, the fixed load
 * address, the 32->64-bit transition -- belongs to boot-shim's own phys.ld
 * now; physload never runs before paging and long mode are already up.
 */

ENTRY(obi_main)

PHDRS {
    code     PT_LOAD    FLAGS(5);  /* R|X */
    rodata   PT_LOAD    FLAGS(4);  /* R */
    data     PT_LOAD    FLAGS(6);  /* R|W */
    note     PT_NOTE    FLAGS(0);
    dynamic  PT_DYNAMIC FLAGS(6);  /* R|W; required for an ET_DYN image */
}

SECTIONS {
    . = 0;
    PROVIDE_HIDDEN(__executable_start = .);

    . = ALIGN(8);
    .text : {
        *(.text*)
    } :code

    /* -fno-plt (config("phys-module")) keeps this empty in practice; kept
     * as an ordinary orphan-eligible section rather than asserted empty,
     * since unlike phys.ld's boot-shim build this one is genuinely PIC and
     * a future relaxation of that flag should not need a script edit. */
    .plt : { *(.plt) }
    .plt.got : { *(.plt.got) }

    /*
     * The segments must not share a page: ElfImage's load policy maps each
     * PT_LOAD separately and requires page-disjoint mappings, since R|X and
     * R contents cannot share one page under distinct permissions. This is
     * the same convention kernel.ld follows between its regions.
     */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .rodata : {
        *(.rodata*)
    } :rodata

    /*
     * physload's own build ID, kept locatable at runtime rather than only
     * in the section-header table (which strip-sections drops from the
     * item physload actually ships as).  Assigned to both :rodata (so it
     * is physically loaded) and :note (so a PT_NOTE-walking tool finds it
     * the ordinary way too).  ModuleLoader compares a loaded module's
     * PT_INTERP against this before calling into it
     * (ours/phys/build-id.hpp).
     */
    . = ALIGN(8);
    .note.gnu.build-id : {
        PROVIDE_HIDDEN(__build_id_note_start = .);
        KEEP(*(.note.gnu.build-id))
        PROVIDE_HIDDEN(__build_id_note_end = .);
    } :rodata :note

    /* ET_DYN scaffolding for the relative relocations boot-shim applies.
     * No symbolic content: PHYS_EXPORT and hidden-by-default visibility
     * mean nothing here is meant to be looked up by name (unlike
     * physload_module()'s own dynsym/hash, which a module's imports are
     * resolved against by name at bind time -- physload has no binder). */
    .dynsym : { *(.dynsym) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .hash : { *(.hash) } :rodata
    .rela.dyn : { *(.rela.dyn) } :rodata

    /* rodata -> data is the second permission boundary. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic : { *(.dynamic) } :data :dynamic

    .got : { *(.got) } :data
    .got.plt : { *(.got.plt) } :data

    /*
     * physload's module ABI.  gen-phys-exports.py emits one
     * {name, address} row per exported symbol; ModuleLoader scans this
     * range to bind a module's undefined symbols (ours/phys/exports.hpp).
     * KEEP: nothing references the rows, so --gc-sections would drop them.
     *
     * :data, not :rodata: each row's second word is an absolute pointer
     * (the exported symbol's address), which under -fPIC the linker emits
     * as an R_X86_64_RELATIVE relocation -- boot-shim's relative-relocation
     * pass fixes these up the same as any other pointer in physload's data,
     * once it knows where physload actually landed.  A relocation target
     * has to sit in a writable segment for the fixup to land; :rodata (no
     * W flag) made the linker refuse to emit it at all.
     */
    . = ALIGN(8);
    .phys.exports : {
        PROVIDE_HIDDEN(__phys_exports_start = .);
        KEEP(*(.phys.exports))
        PROVIDE_HIDDEN(__phys_exports_end = .);
    } :data

    . = ALIGN(8);
    .ccrt_init : {
        PROVIDE_HIDDEN(__ccrt_init_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__ccrt_init_end = .);

        ASSERT(ALIGNOF(.ccrt_init) == 8 || SIZEOF(.ccrt_init) == 0,
               ".ccrt_init overalignment -> maybe padding gaps");
    } :data

    .data : {
        . = ALIGN(8);
        *(.data*)
    } :data

    . = ALIGN(8);
    .bss : {
        PROVIDE_HIDDEN(__bss_start = .);
        *(.sbss*) *(.bss* COMMON)
        PROVIDE_HIDDEN(__bss_end = .);
    } :data
    PROVIDE_HIDDEN(__executable_end = .);

    /DISCARD/ : {
        *(.note.GNU-stack)
    }
}
