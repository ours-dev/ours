/// SPDX-License-Identifier: MIT
/// Positive test: all code-patching macro patterns that should assemble cleanly.
///
/// `code-patches.inc` is the ONLY generated file — it contains both
/// CODE_PATCH_MAX_<suffix> (max blob size) and CASE_ID_<suffix>
/// (auto-assigned numeric ID).  No hand-written arch header needed.
///
/// The assembly code connects a patch site to a case by passing the case
/// suffix to .code_patch_end / .code_patching.blob.

#include <arch/asm.hpp>
#include <lib/code-patching/asm.hpp>
#include "code-patches.inc"

.text

// ── Scenario A: alt (TEST_SMALL=5) > code (3 bytes) ────────────────────
// Expect: 2 bytes of NOP padding, max_size = 5
.function test_alt_larger_than_code, global, align=16
    .code_patch_start patch_alt_larger
        .fill 3, 1, 0x90
    .code_patch_end patch_alt_larger, TEST_SMALL
    ret
.end_function

// ── Scenario B: alt (TEST_LARGE=32) >> code (7 bytes) ──────────────────
// Expect: 25 bytes of NOP padding, max_size = 32
.function test_alt_much_larger, global, align=16
    .code_patch_start patch_alt_much_larger
        .fill 7, 1, 0x90
    .code_patch_end patch_alt_much_larger, TEST_LARGE
    ret
.end_function

// ── Scenario C: alt (TEST_SMALL=5) == code (5 bytes) ───────────────────
// Expect: no padding, max_size = 5
.function test_alt_equals_code, global, align=16
    .code_patch_start patch_alt_equals
        .fill 5, 1, 0x90
    .code_patch_end patch_alt_equals, TEST_SMALL
    ret
.end_function

// ── Scenario D: TEST_NO_ALT has no CODE_PATCH_MAX defined ──────────────
// Expect: no padding, max_size = code_size, case_id from CASE_ID_TEST_NO_ALT
.function test_no_alternative, global, align=16
    .code_patch_start patch_no_alt
        .fill 7, 1, 0x90
    .code_patch_end patch_no_alt, TEST_NO_ALT
    ret
.end_function

// ── Scenario E: .code_patching.blob ────────────────────────────────────
// Expect: 16 bytes of NOP padding (from CODE_PATCH_MAX_TEST_BLOB)
.function test_hermetic_blob, global, align=16
    .code_patching.blob test_hermetic_blob, TEST_BLOB
.end_function

// ── Scenario F: code (8 bytes) > alt (TEST_SMALL=5) ───────────────────
// Expect: max_size = code_size (8), no padding
.function test_code_larger_than_alt, global, align=16
    .code_patch_start patch_code_larger
        .fill 8, 1, 0x90
    .code_patch_end patch_code_larger, TEST_SMALL
    ret
.end_function

// ── Scenario G: two patches in one function ────────────────────────────
// Expect: two independent patch sites
.function test_multiple_patches, global, align=16
    .code_patch_start patch_first
        .fill 3, 1, 0x90
    .code_patch_end patch_first, TEST_SMALL

    .fill 4, 1, 0x90

    .code_patch_start patch_second
        .fill 8, 1, 0x90
    .code_patch_end patch_second, TEST_LARGE

    ret
.end_function
