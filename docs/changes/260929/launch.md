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

## The planner became `Plan::build()` and `Plan::make()`, and `make()` is written (2026-09-29)

**Files**
- `system/lib/launch/include/launch/plan.hpp` — `plan_mappings()` and its out-parameter became the class `Plan`: `build()` returns `ustl::Result<Plan, Status>`, the storage is `std::array`, and the only writers are `add_mapping()`, `add_copy()` and `set_private_size()`
- `system/lib/launch/src/plan.cpp` — `Plan::make()`, the body of the planner, written; the specification comment above it is unchanged apart from the name
- `system/lib/launch/test/plan_test.cpp` — the tests call `Plan::build()`; 28 of them now (the earlier entry said 29)
- `.agents/tasks/launch-plan-exercise/` — the exercise brief, the review of the user's draft, and a diagram of a writable segment

**Symptom**
The earlier entry above describes a free function `plan_mappings(segments, relro, file_size, plan)` that fills a caller-owned `Plan`. Nothing stops a caller from reading a `Plan` that the function left half written, and the fixed-size arrays were C arrays with the counts held by the caller's code. The body was a stub, so every test that needed a mapping failed.

**Cause**
The free function with an out-parameter is C style: the invariant "a Plan is whole or does not exist" was in the caller's head, not in a type. `Plan` owns that invariant now. Its constructor is private, so `build()` is the only way to get one, and `build()` throws away a plan that `make()` did not finish, which is why `make()` may stop part way and needs no cleanup path.

The body has two passes because of what a hostile file can do to the arithmetic. Pass 1 only judges: sizes, flags, the page offset of `vaddr` against `file_offset`, the bytes being in the file, `vaddr + mem_size` and its rounding up not wrapping, no zero-fill of a file mapping, order and shared pages through `first < previous_end`, and RELRO being a prefix of a writable segment. Pass 2 then adds and subtracts without a check on every line. The two checks that are easy to get wrong are written so that they cannot wrap themselves: the bytes-in-file test compares `file_size` with what is left of the file after `file_offset`, since `file_offset + file_size > file_size` wraps for an offset near 2^64 and then passes; and the address check compares against `UINT64_MAX - b` instead of testing `a + b < a`.

**Change**
The writable branch is the core. Before, in the draft the user wrote, every segment became one `Source::File` mapping:

```cpp
mapping.vmo_offset = ustl::mm::align_up(segment.file_offset, PAGE_SIZE);
```

which rounds the wrong way (the page holding the first byte starts at the rounded-down offset) and mapped writable data straight from the shared file. Now a writable segment gets `set_private_size(i, end - first)`, one `Copy{vmo_offset = vaddr - first}` when `file_size` is not 0, and `Source::Private` mappings whose `vmo_offset` is `page - first` in that segment's own VMO. With RELRO the VMO is cut at `align_down(vaddr + size)`: the whole pages before it are `Perm::R`, the rest `Perm::RW`, and either part is left out when empty.

Considered and left: mapping the data from a copy-on-write snapshot of the file VMO, of the kind Zircon's `ld` makes for data VMOs with `ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE` (`sdk/lib/ld/include/lib/ld/remote-abi-heap.h`). The kernel only supports slice children (`VmoDispatcher::create_child()` answers `NotSupported` for the rest), and the planner would still need to zero the tail of the last file page, so the explicit `Copy` stays until snapshot children exist. Decided by the user on 2026-09-29.

**Reference**
Zircon `elfldltl` load planning (`LoadInfo`, `RelroBounds`), whose layout expects RELRO at the start of a writable segment, and glibc `_dl_protect_relro`; here the protection is final at mapping time.

**Verification**
- Host tests: `prebuilt/ninja/ninja -C out/phys-host-tests`, then the `plan` binary: 28 of 28 pass.
- `clang-tidy -p out/phys-host-tests system/lib/launch/src/plan.cpp`: no diagnostics. `scripts/format_cpp.py --check`: clean.
- Not booted: nothing uses the library yet, so `ours-verify` is not run for this change.

**Deliberately not done**
- The user's hand-written `make()`. The plan was for the user to write it under a time limit and be reviewed; the user's draft reached 13 of 28 tests, was reviewed (`.agents/tasks/launch-plan-exercise/REVIEW.md`), and then the user asked for it to be written, on the view that a program-header parser teaches little. So this `make()` is not the user's and there is no row in `docs/authored.md`. The milestone still has no hand-written core function.
- The executor that does what a `Plan` says (`vmo_create`, `vmo_write`, `vma_map`), placement of the image, the stack, and ASLR, as in the earlier entry.
- Copy-on-write for writable segments, above.

**How it was found**
None. This is the specified feature, written against the tests.

**The user's prediction**
None. The user did not predict before the exercise; the draft's 13 of 28 is recorded here instead of a comparison.

**Three questions you should be able to answer**
- Why is `file_offset + file_size > file_size` not a safe bounds check, and what does `make()` compare instead?
- Why does a writable segment need its own private VMO whose size is recorded apart from the `Copy` and the mappings?
- Why is `Copy.vmo_offset` `vaddr - first` and not 0, while the first `Source::Private` mapping's `vmo_offset` is 0?
