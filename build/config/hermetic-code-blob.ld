/* Copyright 2023 The Fuchsia Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * Link one position-independent leaf routine at address zero.  Assertions
 * reject data, unresolved dynamic relocations, and an entrypoint that is not
 * the first instruction.  The resulting ELF is converted to a raw binary by
 * code_patching_hermetic_alternative().
 */
ENTRY(HERMETIC_ENTRY)

SECTIONS {
  . = 0;

  .text : { *(.text.entry) *(.text*) }
  ASSERT(HERMETIC_ENTRY == 0,
         "hermetic code blob requires entry point at beginning of code")

  .rodata : { *(.rodata*) }
  ASSERT(DEFINED(HERMETIC_RODATA_OK) || SIZEOF(.rodata) == 0,
         "RODATA not allowed in this hermetic code blob")

  .relr.auth.dyn : { *(.relr.auth.dyn) }
  .relr : { *(.relr.*) }
  .rela : { *(.rela.*) }
  .rel : { *(.rel.*) }
  ASSERT(SIZEOF(.relr.auth.dyn) + SIZEOF(.relr) + SIZEOF(.rela) +
             SIZEOF(.rel) == 0,
         "hermetic code blob cannot use dynamic relocations")

  /DISCARD/ : { *(.eh_frame*) }

  .other : { INPUT_SECTION_FLAGS(SHF_ALLOC) *(*) }
  ASSERT(SIZEOF(.other) == 0,
         "unexpected SHF_ALLOC sections not .text* or .rodata*")
}
