ENTRY(MAIN_ENTRY)

SECTIONS {
    . = 0;

    PROVIDE_HIDDEN(__executable_start = .);

    /* Build ID */
    .interp : { *(.interp) }
    .dynamic : { *(.dynamic) }

    /*
     * Page boundary inside the image head: .dynamic is writable, everything
     * after it here is read-only, and ElfImage's load policy demands that
     * neighbouring PT_LOADs with different permissions own disjoint pages.
     */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .relr.auth.dyn : { *(.relr.auth.dyn) }
    ASSERT(SIZEOF(.relr.auth.dyn) == 0, ".relr.auth.dyn is not supported")
    .relr.dyn : { *(.relr.dyn) }
    ASSERT(SIZEOF(.relr.dyn) == 0, ".relr.dyn is not supported")
    /* The static-PIE relocation table: ElfImage::relocate() walks it at load
     * time to apply every R_X86_64_RELATIVE entry. */
    .rela.dyn : { *(.rela.dyn) *(.rela.*) }

    .note.gnu.build-id : {
        PROVIDE_HIDDEN(__build_id_note_start = .);
        *(.note.gnu.build-id)
        PROVIDE_HIDDEN(__build_id_note_end = .);
        /*
         * Record the build ID size, without the note header (including name)
         * of 16 bytes.  This is used below.
         */
        HIDDEN(__build_id_size = ABSOLUTE(__build_id_note_end - __build_id_note_start) - 16);
    }

    /*
     * Each PT_LOAD must own whole pages: ElfImage's load policy (shared with
     * physload's module loader) maps segments independently and requires
     * page-disjoint mappings, since neighbours with different permissions
     * cannot share one. Page-align every permission boundary, not just 8.
     */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    PROVIDE_HIDDEN(__code_start = .);
    .text : {
        *(SORT_BY_ALIGNMENT(.text*))
    }
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    PROVIDE_HIDDEN(__code_end = .);

    PROVIDE_HIDDEN(__rodata_start = .);
    .rodata : {
        *(SORT_BY_ALIGNMENT(.rodata*))
    }

    /*
     * The sampling profiler unwinds kernel C++ frames from CFI emitted into
     * .eh_frame (PC-relative, so no runtime relocations are needed). The
     * section must be part of the loaded image; it is read-only and never
     * written, so it lives in the rodata segment.
     */
    . = ALIGN(8);
    .eh_frame : {
        PROVIDE_HIDDEN(__eh_frame_start = .);
        KEEP(*(.eh_frame))
        KEEP(*(.eh_frame.*))
        PROVIDE_HIDDEN(__eh_frame_end = .);
    }

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    PROVIDE_HIDDEN(__rodata_end = .);

    PROVIDE_HIDDEN(__data_start = .);
    .data : {
        /* logz4::Target registry (Target::all()). Writable, unlike the
         * command and counter-descriptor registries in RELRO below: each
         * record holds its target's runtime level floor, which `log level`
         * stores into. KEEP because nothing references a target that no
         * surviving code logs to, and it should still be listed -- and
         * settable -- by name. The bound names are the __start_/__stop_ pair
         * a default layout would synthesize for this C-identifier section,
         * so src/target.cpp reads the same symbols everywhere. */
        PROVIDE_HIDDEN(__start_logz4_targets = .);
        KEEP(*(logz4_targets))
        PROVIDE_HIDDEN(__stop_logz4_targets = .);

        *(SORT_BY_ALIGNMENT(.data*))
    }
    /* Aligned to Page Size */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    PROVIDE_HIDDEN(__data_end = .);

    /*
     * LLD only makes particular named sections part of PT_GNU_RELRO, so
     * all the special RELRO sections have to be merged into one output section.
     */
    PROVIDE_HIDDEN(__relro_start = .);
    .got : { *(.got*) }
    .data.rel.ro : ALIGN(8) {
        PROVIDE_HIDDEN(__commands_start = .);
        /* KEEP, like every other registry section in this file. A GKTL_COMMAND
         * record is referenced by nothing -- being found by walking this range
         * is the entire mechanism -- so without KEEP, --gc-sections collects
         * every one of them and __commands_start == __commands_end. The
         * registry was empty at run time for exactly that reason, which is
         * invisible from the console: an unknown command and a command that
         * was collected both do nothing. */
        KEEP(*(SORT_BY_ALIGNMENT(.gktl.commands*)))
        PROVIDE_HIDDEN(__commands_end = .);

        PROVIDE_HIDDEN(__gktl_counter_desc_start = .);
        *(.gktl.counter.descs*)
        PROVIDE_HIDDEN(__gktl_counter_desc_end = .);

        /* TODO(https://fxbug.dev/384994543): Alignment should actually be 8 if present. */
        *(.data.rel.ro*)
        ASSERT(ALIGNOF(.data.rel.ro) <= 32 || SIZEOF(.data.rel.ro) == 0,
                       ".data.rel.ro overalignment -> padding gaps");
    }
    /* Aligned to Page Size */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    PROVIDE_HIDDEN(__relro_end = .);

    PROVIDE_HIDDEN(__bss_start = .);
    .bss : ALIGN(16) {
        *(SORT_BY_ALIGNMENT(.bss*))

        . = ALIGN(16);
    }
    /* Aligned to Page Size */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    PROVIDE_HIDDEN(__bss_end = .);

    /*
     * Reclaimed once initialization is over -- and therefore the tail of the
     * image, not a slice out of its middle.  physboot places the whole image
     * in one contiguous physical allocation, so the only parts the kernel can
     * hand back to the page allocator without punching a hole in that
     * allocation are its two ends; the tail is the one that merges with
     * whatever free memory follows.
     */
    PROVIDE_HIDDEN(__init_start = .);

    PROVIDE_HIDDEN(__init_code_start = .);
    .init.text : AT(ADDR(.init.text)) {
        *(SORT_BY_ALIGNMENT(.init.boot.head))
        *(SORT_BY_ALIGNMENT(.init.boot*))
        *(SORT_BY_ALIGNMENT(.init.text*))
    }
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    PROVIDE_HIDDEN(__init_code_end = .);

    PROVIDE_HIDDEN(__init_rodata_start = .);
    .init.rodata : AT(ADDR(.init.rodata)) {
        /* hsien test records */
        PROVIDE_HIDDEN(__hsien_tests_start = .);
        KEEP(*(SORT_BY_ALIGNMENT(.hsien.tests)))
        PROVIDE_HIDDEN(__hsien_tests_end = .);

        /* gktl::InitHook */
        PROVIDE_HIDDEN(__init_hook_start = .);
        KEEP(*(SORT_BY_ALIGNMENT(.gktl.init_hook*)))
        PROVIDE_HIDDEN(__init_hook_end = .);

        /* gktl::Counter descriptors */
        PROVIDE_HIDDEN(__gktl_counter_desc_start = .);
        KEEP(*(SORT_BY_ALIGNMENT(.gktl.counter.descs)))
        PROVIDE_HIDDEN(__gktl_counter_desc_end = .);

        /* task::sched discipline registry. Collected here, before the
         * catch-all below, and *not* named .init.rodata.something: this used
         * to live in its own INSERT AFTER fragment as .init.rodata.schedules,
         * where the wildcard on the next line matched it first and swallowed
         * every entry into the body of this section. The bounds then sat past
         * the end of what they were supposed to delimit, and Scheduler::init()
         * read an empty registry. */
        PROVIDE_HIDDEN(__schedules_start = .);
        KEEP(*(SORT_BY_ALIGNMENT(.task.schedules)))
        PROVIDE_HIDDEN(__schedules_end = .);

        *(SORT_BY_ALIGNMENT(.init.rodata*))
    }
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    PROVIDE_HIDDEN(__init_rodata_end = .);

    PROVIDE_HIDDEN(__init_data_start = .);
    .init.data : AT(ADDR(.init.data)) {
        /* ours::CpuLocal begin */
        . = ALIGN(CONSTANT(MAXPAGESIZE));
        PROVIDE_HIDDEN(__cpu_local_start = .);

        /* gktl::Counter begin */
        /*
         * KEEP, and not decoration. Nothing references a counter's storage by
         * name: GKTL_COUNTER_DEFINE emits COUNTER_STORAGE_##VAR into this
         * section and the Counter object only ever holds &COUNTER_DESCRIPTOR,
         * reaching the slot as COUNTERS_START[descriptor index]. With
         * --gc-sections on and no referrer, every one of these input sections
         * was collected -- __attribute__((used)) stops the *compiler* from
         * dropping it, not the linker. The descriptors survive because they
         * do have a referrer, and because their own placement below is
         * already KEEP()ed.
         *
         * The consequence was not merely that counters read zero. The section
         * came out empty, so __gktl_counter_start coincided with
         * __cpu_local_start and COUNTERS_START[i] indexed into whatever the
         * catch-all glob below had placed at the front of the per-CPU area:
         * every read returned an unrelated per-CPU word, and every add()
         * corrupted one. SpinLock::num_held_cpu() reporting 0x0400b000_00000000
         * plus a working low-half count is what that looks like from outside.
         */
        PROVIDE_HIDDEN(__gktl_counter_start = .);
        KEEP(*(.init.data.cpu_local.gktl.counter.storage))
        PROVIDE_HIDDEN(__gktl_counter_end = .);
        /* gktl::Counter end */

        *(SORT_BY_ALIGNMENT(.init.data.cpu_local*))

        PROVIDE_HIDDEN(__cpu_local_end = .);
        /* ours::CpuLocal end*/

        . = ALIGN(CONSTANT(MAXPAGESIZE));
        *(SORT_BY_ALIGNMENT(.init.data*))
    }

    /*
     * .init_array is writable, so .ccrt_init belongs beside .init.data
     * rather than after .init.rodata: two PT_LOADs with different
     * permissions may not share a page (see the note above .text).
     *
     * It still gets a page of its own, because it is also the RELRO that
     * physboot maps read-only once it has relocated the image -- and it can
     * only do that for whole pages it does not have to share with the
     * writable data ahead of it.
     */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .ccrt_init : {
        PROVIDE_HIDDEN(__ccrt_init_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)
               SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__ccrt_init_end  = .);

        ASSERT(ALIGNOF(.ccrt_init) == 8 || SIZEOF(.ccrt_init) == 0,
               ".ccrt_init overalignment -> maybe padding gaps");
    }
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    PROVIDE_HIDDEN(__init_data_end = .);

    PROVIDE_HIDDEN(__init_end  = .);

    PROVIDE_HIDDEN(__executable_end = .);

    ASSERT(__init_end == __executable_end, "the `.init*` sections must be the tail of the kernel image")

    /* Non-allocated section needs to be protected from GC with BFD ld. */
    .code-patches 0 : {
        KEEP(*(.code-patches))
    }
}
