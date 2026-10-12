# unwind changes

## One CIE the parser declines costs only its own FDEs, and "zPLR" and "zRS" CIEs parse (2026-10-11)

**Files**
- `kernel/lib/unwind/src/fde_index.cpp` — `FdeIndex::build()` steps over an FDE whose CIE does not parse, or which does not parse itself, instead of failing the whole build.
- `kernel/lib/unwind/include/unwind/fde_index.hpp` — `build()`'s contract: false only for broken record framing or a refusing sink.
- `kernel/lib/unwind/src/cfi.cpp` — `parse_cie()` skips a 'P' personality by its format, ignoring the indirect bit, and accepts 'S'.
- `kernel/lib/unwind/include/unwind/cfi.hpp` — `Cie::signal_frame`.
- `kernel/lib/unwind/test/cfi_test.cpp` — `EhFrameBuilder::emit_cie_with()` and `use_cie()`; three tests.
- `docs/repairs/261005/unwind.md` — item 1 removed.

**Symptom**
Latent: the kernel's `.eh_frame` holds one "zR" CIE today. But emitting the kernel's assembly CFI there brings a "zRS" CIE (`interrupt_common`, `paranoid_common`, `.cfi_signal_frame`), and any object with a personality brings "zPLR". Either one made `parse_cie()` return false, `FdeIndex::build()` return false, and the sampler log "malformed .eh_frame; samples will not unwind": every kernel sample of every session at depth 1, from one CIE.

**Cause**
Two separate mistakes, compounding.
- The index treated "a record I cannot read" as "a section I cannot read". The two are different: identify_entry() and the length check have already told the walk where the record ends, so stepping over it is as safe as stepping over one it read. Only broken framing, a record whose extent is unknown, makes the rest of the section untrustworthy.
- `parse_cie()` refused two common augmentations. 'S' it did not know at all (`default: return false`). 'P' it read with `encoded_pointer()`, which refuses DW_EH_PE_indirect (0x80), the bit that says the value is the address of the personality rather than the personality. The parser never uses the value, only steps over it, and its size depends on the format bits alone; clang emits 0x9B (indirect|pcrel|sdata4) in every "zPLR" CIE.

**Change**
In `FdeIndex::build()`, before and after:

```cpp
// Before: any CIE or FDE that did not parse ended the build.
if (!parse_cie(section, cie_offset, &cached_cie)) {
    return false;
}

// After: the result is cached, and only this CIE's FDEs are left out.
if (cie_offset != cached_cie_offset) {
    cached_cie_parsed = parse_cie(section, cie_offset, &cached_cie);
    cached_cie_offset = cie_offset;
}
if (cached_cie_parsed && parse_fde(section, offset, cached_cie, &fde) && fde.pc_range != 0) {
    // index it
}
```

In `parse_cie()`: `cursor.encoded_pointer(personality_encoding & ~DW_EH_PE_INDIRECT, datum_base)` for 'P', and `case 'S': cie.signal_frame = true;`. 'S' needs no other change: the unwinder looks up every pc as it is (`Unwinder::locate()` calls `find(pc)`), never pc - 1, so it already treats every frame's pc as exact, which is what a signal frame asks for.

**Reference**
Fuchsia's `src/lib/unwinder/cfi_module.cc`, `CfiModule::DecodeCie()`: reads 'P' through `ReadEncodedAndAdvance` and records 'S' as `is_signal_frame`. Linux's in-kernel ORC unwinder sidesteps CFI parsing at run time altogether; the DWARF unwinders that do parse it (libunwind's `CFI_Parser::parseCIE()`) also step over the personality by its encoding and record 'S'.

**Verification**
- clang-format clean on the changed lines; clang-tidy (`out/bringup.x86`) reports nothing on them.
- In `.worktrees/wip-proc-self` at baseline `752c5604` plus only this change: build ok, `dbg PASS 35 passed, 3 skipped, 0 failed`, `rel PASS 33 passed, 8 skipped, 0 failed, 6/6 required`, host tests 144 passed; the three new tests pass (`ParseCie.StepsOverAnIndirectPersonality`, `ParseCie.AcceptsASignalFrame`, `FdeIndex.DropsOnlyTheFdesOfACieItDeclines`).
- With `fde_index.cpp` at baseline: `FdeIndex.DropsOnlyTheFdesOfACieItDeclines` fails. With `cfi.cpp` at baseline: both `ParseCie` tests fail.

**Deliberately not done**
- The kernel's assembly CFI still goes to `.debug_frame` (`kernel/debug_frame.S`); moving it to `.eh_frame` is its own change, which this one makes safe.
- No count of the FDEs left out: `build()` keeps its signature, and nothing would read the count yet.

**How it was found**
The audit of 2026-10-05 (unwinder-2), recorded as `docs/repairs/261005/unwind.md` item 1.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is it safe to step over a record the parser cannot read, but not over one whose length runs past the section?
2. What does the indirect bit in a pointer encoding say, and why does it not change how many bytes the value takes?
3. What does a signal frame's pc mean, and why does this unwinder need no change to handle one correctly?

## slot_address() multiplies the CFI's factored offset by the data alignment in uint64 (2026-10-11)

**Files**
- `kernel/lib/unwind/src/unwinder.cpp` — `slot_address()`.
- `docs/repairs/261005/unwind.md` — item 4 removed.

**Symptom**
Latent: only a corrupt `.eh_frame` reaches it, and the kernel's is its own read-only data. A `DW_CFA_offset` with a factored offset of 0x2000000000000000 and a data alignment of -8 made `slot_address()` overflow an int64 multiply, which is undefined behaviour: under the ubsan-trap variant, a trap inside the sampling interrupt.

**Cause**
Every other product of CFI values in the unwinder is unsigned, so it wraps with defined results, or goes through `__builtin_mul_overflow` (`DW_CFA_def_cfa_sf` and friends). `slot_address()` alone multiplied `rule.offset` by `cie.data_align` as int64, though both come straight from the CFI.

**Change**
Before: `std::int64_t const bytes = rule.offset * cie.data_align; return cfa + static_cast<std::uint64_t>(bytes);`. After: `std::uint64_t const bytes = static_cast<std::uint64_t>(rule.offset) * static_cast<std::uint64_t>(cie.data_align); return cfa + bytes;`. Two's-complement multiplication gives the same low 64 bits signed or unsigned, so every product that fitted is unchanged; one that did not now wraps to an address the stack window refuses, and the step ends as a prefix. Rejected: `__builtin_mul_overflow` with a BadCfi result, which would change `slot_address()`'s signature at its six call sites for an outcome the reader already produces.

**Reference**
None, this project's own code.

**Verification**
clang-format clean; clang-tidy (`out/bringup.x86`) reports nothing on the lines. In `.worktrees/wip-proc-self` at baseline `34ada8bb` plus only this change: dbg PASS (35 passed), rel PASS (33 passed, 6/6 required), host tests 144 passed; profile and the sampler stress test record as before (`216 kernel samples`, `349 records checked`). No test added: undefined behaviour shows only under a sanitizer, and the host tests are built without one.

**Deliberately not done**
None.

**How it was found**
The audit of 2026-10-05 (unwinder-6), recorded as `docs/repairs/261005/unwind.md` item 4.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does a uint64 multiply give the same result as an int64 one whenever the int64 one does not overflow?
2. Why is signed overflow worse than a wrong address here, given that the address is checked anyway?
3. Why could no test in the current host build catch this bug?

## FdeIndex states the contract find() keeps: entries with no overlapping ranges (2026-10-11)

**Files**
- `kernel/lib/unwind/include/unwind/fde_index.hpp` — `init()`'s contract.
- `kernel/lib/unwind/src/fde_index.cpp` — `find()`'s comment.
- `docs/repairs/261005/unwind.md` — item 3 removed.

**Symptom**
The header promised that "overlapping ranges are resolved by first match, which favours exact-range entries", and `find()`'s comment repeated it. Neither held: with {[0x1000,0x1100), [0x1010,0x1020)} `find(0x1050)` returns nothing although the outer range covers it, and of two ranges with one start it returns the later in the array, the wider when sorted by end.

**Cause**
`find()` takes the rightmost entry whose `pc_begin <= pc` and checks containment for that entry alone. That is correct exactly when no two ranges overlap, which is what a linker's `.eh_frame` gives: the kernel's 1475 FDEs have no overlap and no shared start (checked against `llvm-objdump --dwarf=frames` in the audit). The comment described a capability nobody had built.

**Change**
The contract now says what `find()` does: entries sorted by `pc_begin`, with no overlapping ranges, and what happens if there are (the two cases above). It also says what supporting overlap would cost: each entry would carry the greatest end of the entries before it, so the search could step left past nested ranges. Rejected: building that, since no section this index reads has overlapping FDEs.

**Reference**
None, this project's own code.

**Verification**
Comments only. In `.worktrees/wip-proc-self` at baseline `5d5c000d` plus only this change: dbg PASS, rel PASS (6/6 required), host tests 144 passed.

**Deliberately not done**
Overlap support, and a check in `init()` that refuses overlapping input: the core is host-testable and has no assertion of its own, and the only producer of the array, the sampler's `build_fde_table()`, sorts a linker's output.

**How it was found**
The audit of 2026-10-05 (unwinder-5), recorded as `docs/repairs/261005/unwind.md` item 3.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does checking only the rightmost entry with `pc_begin <= pc` suffice for non-overlapping ranges, and fail for nested ones?
2. What extra field per entry would let the search handle nesting, and how would it be used?
3. Why can a comment that promises more than the code does be worse than no comment?
