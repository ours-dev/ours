# launch-plan-exercise: review of the user's Plan::make()

**Date**: 2026-09-29

## Scope and method
Reviewed `Plan::make()` in `system/lib/launch/src/plan.cpp` (lines 69-118) as it stood when the user said they were done, against the specification comment above it. The function was renamed from `fill()` to `make()` by the user before the exercise started; the comments in the header, the test and the specification still say `fill()` and are renamed in step 4. Time taken: not measured (the user started the clock themselves). Tools: the host tests (`prebuilt/ninja/ninja -C out/phys-host-tests`, then the `plan` binary), `modern-cpp-development`, and the repo's `.clang-format` column limit (100). Every failure below was run, not read: 9 of 28 tests pass and 19 fail. The failures group into three causes: writable segments never take the private-VMO path (9 tests), the file offset is rounded in the wrong direction (2 tests), and 8 of the input checks are missing (8 tests).

## What is done well
- `plan.cpp:75-80`: the empty and too-many checks come first and return the two different statuses the specification asks for (`InvArgs` versus `OutOfRange`). `RefusesNoSegments` and `RefusesTooManySegments` pass.
- `plan.cpp:82-95`: validating every segment in a loop of its own before producing anything is a good shape. Nothing has to be undone when a later segment turns out to be bad, and it leaves room for the checks that need two segments at once.
- `plan.cpp:86`: `flags & (PF_R | PF_W | PF_X)` ignores the bits that are not access bits, so `0xf0000000` counts as no access. `RefusesNoAccessAtAll` passes because of it.
- `plan.cpp:99-100`: the first page and the size are computed as `first` and `end - first`, the same names the specification uses, so the code can be read against the text.
- `plan.cpp:73`: `using ours::Status;` keeps the status names short without a `using namespace`.

## High severity
### H1. Writable segments never get a private VMO, a Copy or a Private mapping
`plan.cpp:97-115`. The loop builds one `Source::File` mapping for every segment, whatever its flags. The specification has a second branch for PF_W (private VMO of `end - first` bytes via `set_private_size`, one `Copy` when `file_size` is not 0, and `Source::Private` mappings with a `vmo_offset` measured from `first`), and none of it exists. Mapping a writable segment straight from the file would let the program write into the shared executable file, which is the one thing `Source::File` is documented never to do. Nine tests fail because of it: `TypicalLayout`, `NoRelroIsOneWritableMapping`, `RelroThatIsNotAWholePageIsNotProtected`, `RelroThatIsTheWholeSegmentLeavesNothingWritable`, `RelroSpanningPagesRoundsItsEndDown`, `ProtectionFollowsTheFlags` (its writable half), `WritableSegmentWithNothingInTheFileIsAllZero`, `TwoWritableSegmentsEachHaveTheirOwnVmo` and `RelroBelongsToTheSegmentItStartsIn`. Fix: add the writable branch; the relro parameter is never read, so the RELRO split is part of this branch, not a separate feature.

### H2. Overflow is not checked anywhere, and the check that is there can itself overflow
`plan.cpp:92`, `plan.cpp:100`. `segment.file_offset + segment.file_size > file_size` wraps for a `file_offset` near 2^64, so a segment whose bytes are nowhere in the file passes as if they were inside it. `segment.vaddr + segment.mem_size` at line 100 wraps the same way, and `align_up` of a value near 2^64 wraps a third time; in each case the result is a small `end`, so `end - first` is a huge or nonsense size that would go to `vma_map`. The input is a file from disk and is not trustworthy, which is the whole reason the specification lists this rule. `RefusesArithmeticThatOverflows` fails. Fix: each of the three sums must be proved not to wrap before its value is used, and the proof must not itself use the sum.

### H3. Segments that are out of order or share a page are accepted
Nothing in the function compares one segment with the one before it. `RefusesSegmentsOutOfOrder` and `RefusesSegmentsThatShareAPage` fail. Two segments in one page would produce two mappings of the same page with different protections, and a page has only one. This is also the check that needs the ends of the previous segment, so it belongs in the same pass as `end` is worked out.

### H4. RELRO is ignored completely
The `relro` parameter is never read. The four RELRO refusals fail (`RefusesRelroThatDoesNotStartTheSegment`, `RefusesRelroLongerThanItsSegment`, `RefusesRelroInASegmentThatIsNotWritable`, `RefusesRelroInNoSegment`) because nothing checks that RELRO is a prefix of one writable segment. A parameter the function never reads is a sign that a branch of the specification has not been written.

## Medium severity
### M1. The file offset is rounded up, the specification says down
`plan.cpp:102`. `align_up(segment.file_offset, PAGE_SIZE)` for a segment at file offset 0x1234 gives 0x2000; the page that holds the segment's first byte starts at 0x1000, which is what the vaddr side of the same mapping uses at line 99. The two sides disagree, so the mapping would show the wrong file page at the right address. `SegmentThatStartsMidPageIsMappedFromItsPage` and `SegmentThatRunsIntoTheNextPage` fail with 0x2000 instead of 0x1000. It stayed hidden because the test layouts of the other segments start on a page boundary, where up and down agree. Fix: the same direction as line 99.

### M2. `Mapping mapping;` is left uninitialised, and `segment` is never set
`plan.cpp:98`. `Mapping` has no default member initialisers, so `segment` holds whatever was on the stack, and `add_mapping()` copies it into the Plan. Reading an indeterminate `u32` is undefined behaviour, and the test `NoRelroIsOneWritableMapping` shows the visible effect: `m.segment` is 0 where 3 is expected. `Mapping::segment` is documented as the segment index (and, for Private, whose VMO), so it has to be set on every mapping. Fix: initialise the whole object where it is declared, so that a field forgotten later is a zero, not garbage.

### M3. Not writable and `mem_size != file_size` is accepted
Missing from the validation loop. Pages taken from the file cannot be zero-filled, so such a segment cannot be mapped from the file as it is. `RefusesZeroFillOfAFileMapping` fails.

## Minor and style
- `plan.cpp:86` is 108 columns and `plan.cpp:100` is 103; the limit is 100 (`.clang-format`). The pre-commit hook will refuse both.
- `plan.cpp:70-72`: the "Yours to write" comment is still there; delete it when the function is finished.
- `plan.cpp:78`: `Plan::MAX_SEGMENTS` inside a member of `Plan` can be `MAX_SEGMENTS`.
- `plan.cpp:4`: `#include "ours/status.hpp"` is already brought in by `plan.hpp` (which uses angle brackets), and the include order puts a quoted include between two angle ones.
- `plan.cpp:104-113`: three independent `if`s decide the protection, and the result depends on their order (the last one that matches wins). It is right only because W together with X was refused above. See T2.
- There is not one comment inside `make()`. Comments in this tree say why (which rule of the specification a check enforces, what goes wrong without it); the specification above the function says what, so a check with no comment cannot be matched to its rule by a reader. Eight of the rules have no code at all yet, which the missing comments would have shown.
- `modern-cpp-development` items met: trailing return type, braces on every `if`, no ternary, `auto const` for the flags, prelude integer types. Item not met: const-first for `mapping`, which is fine as a mutable object once it is fully initialised (M2).

## Teaching opportunities
### T1. Why two passes, and what would break with one
`plan.cpp:82` and `plan.cpp:97`. The user validates every segment and only then produces mappings. Question for the user: the comment on `Plan::make()` says it may stop part way, because `build()` throws the Plan away on an error. Given that, what does the two-pass shape buy you that one pass would not, and what does it cost? Think about which rules need the previous segment and which need only the segment itself. Compare with how Linux's `load_elf_binary` (fs/binfmt_elf.c) and Zircon's ELF loader treat the program headers, and say where each of them rejects a bad one.

### T2. Why does the chain of ifs at lines 105-113 give the right protection?
Question for the user: walk through PF_X alone, PF_R | PF_X, PF_W alone and PF_R | PF_W, and say what each gives and why. Then say what would happen for PF_W | PF_X if line 86 did not refuse it, and what that says about where the correctness of this chain lives (here or elsewhere).

### T3. Why is `file_offset + file_size > file_size` not a safe bounds check?
Question for the user: give a `file_offset` and `file_size` for which it answers "inside" though the segment is not in the file. Then say in one sentence what a check that cannot wrap has to compare, and why the specification puts "no overflow" before the other checks that use the same sums.

### T4. What does an uninitialised field cost?
`plan.cpp:98`. Question for the user: `add_mapping()` copies the whole `Mapping` into the array. What is copied for the field nobody set, why is that undefined and not just "some number", and what does `= {}` on the array in `plan.hpp:141` protect against that `Mapping mapping;` here does not?

## Suggested follow-up order
1. M2, then M1: two small, local defects; after them the mapping fields are all defined and rounded the right way.
2. H1: the writable branch, with the copy and the private size; RELRO can wait until the branch without it passes.
3. H4 and M3: the remaining input checks that need only one segment or the relro range.
4. H2 and H3: overflow, then order and shared pages; run `RefusesArithmeticThatOverflows` first, since the ordering check reuses the sums.
5. Minor and style last: the line lengths, the stale comment, the include, and a comment on every rule.
6. Answer T1 to T4 before step 4 of the plan (the commit), so that they can go into `docs/authored.md`.
