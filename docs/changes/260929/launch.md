# launch changes

## The launcher library, and the specification and tests of its segment planner (2026-09-29)

**Files**
- `system/lib/launch/` — new library, scaffolded from `tools/templates/cpp_library` with copier
- `system/lib/launch/include/launch/plan.hpp` — the types and the specification of `plan_mappings()`
- `system/lib/launch/src/plan.cpp` — a stub returning `Unimplemented`; the body is the user's to write
- `system/lib/launch/test/plan_test.cpp`, `test/BUILD.gn` — 29 host tests of the specification
- `BUILD.gn` — `//system/lib/launch:tests` joins the top-level test group

**Symptom**
K23b, the user-space launcher, has no code. Its first piece is deciding how to map the PT_LOAD segments of a program: which pages can be mapped straight from the executable BootFS, which must be copied into a VMO of their own, and with what protection.

**Cause**
Missing feature. The decision is worth isolating: the kernel loader maps the whole image as one range with RWX (`kelfload`), while the launcher has to map each segment with its own final protection, and the final protection of a relocated image is not a matter of `p_flags` alone.

**Change**
`plan_mappings(segments, relro, file_size, plan)` turns numbers into a `Plan`: the private VMOs to make, the bytes to copy into them, and the mappings, each with its final protection. It does no I/O, so a host test can check it against a table of layouts.

- **Read-only and executable segments come from the file.** They are mapped from the executable BootFS at `align_down(file_offset)`, and the file's data is page-padded (design U4), so the tail of the last page reads as zero.
- **Writable segments are copied.** A private VMO of the segment's whole page span, the file bytes written at `vaddr - first`, and the rest zero, which is `.bss` for free. Relocations are then written into the private VMO, and only after that is anything mapped.
- **RELRO needs no `protect`.** Relocation happens before mapping, so the RELRO pages are mapped read-only from the start and the rest of the segment read-write, both from the same private VMO. Only whole pages can be read-only: a page that holds RELRO and something else stays writable. RELRO must be a prefix of a writable segment, which is how a linker lays it out; anything else is refused.
- **W^X and the rest are refused rather than repaired**: a segment both writable and executable, zero-filling a file mapping, segments that share a page, bytes past the end of the file, arithmetic that overflows. On any error the plan is left empty.

The specification is the comment on the function; the tests are its check. To be sure the tests are right and not only that they compile, a reference implementation was written and run against them (29 of 29 pass), then removed; eight deliberate mistakes in it (rounding RELRO the wrong way, dropping the page-overlap check, not resetting the plan, allowing W+X, a wrong copy offset, an unrounded file offset, no bounds check on the file, a wrong private offset) are each caught by at least two tests. One test of mine had impossible data (an address and a file offset that disagree within a page), which the reference correctly refused; that is how it was found.

**Reference**
Zircon `elfldltl`'s load planning (`LoadInfo`, `RelroBounds`, and the `-z relro` layout it expects, where RELRO starts a writable segment) and glibc `_dl_protect_relro`. The difference is that the protection is final at mapping time here, since nothing is mapped before relocation.

**Verification**
- The host test target builds and runs: all 29 tests fail against the stub, as they should.
- Not built into either image: the library has no user of it yet, and `ours-verify` covers the images.

**Deliberately not done**
- The executor: the code that does what a plan says (`vmo_create`, `vmo_write`, `vma_map`). It follows once the planner exists.
- Placement and the stack: the planner places nothing; the launcher chooses the base and maps the stack and its guard (`vma_allocate`) separately.
- ASLR: the base is fixed, as it is for the kernel's loader.

**How it was found**
None.

**The user's prediction**
None. The planner is the user's core function for this milestone, to be written by hand under a time limit and reviewed afterwards; it is recorded in `docs/authored.md` when done.

**Three questions you should be able to answer**
- Why can the launcher map every segment with its final protection, when the kernel's loader had to map the whole image writable and executable?
- Why can a page that holds RELRO and something else not be mapped read-only, and what does the planner do with it?
- Why are segments that share a page refused instead of merged into one mapping?
