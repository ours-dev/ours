# ustl changes

## ustl::mm::align_up and align_down are now ustl::round_up and round_down (2026-10-25)

**Files**
- `system/lib/ustl/include/ustl/mem/align.hpp` — the two functions renamed and moved from `ustl::mm` to `ustl`; `ustl::mm::is_aligned` stays where it was
- 27 callers in `kernel/` and `system/lib/launch/` — the new names; `kernel/mm/cpu_local.cpp` and `kernel/mm/vm_object_paged.cpp` also change their `using` declarations

**Symptom**
None, a rename the user asked for.

**Cause**
None, a rename. The standard library has no integer round-up. `std::align` moves a pointer inside a buffer, `std::is_sufficiently_aligned` (C++26) only tests a pointer, and `std::bit_ceil` rounds to a power of two, not to a multiple. So this helper stays in ustl, and the user chose the name `round_*` in `ustl` itself rather than `align_*` in `ustl::mm`.

**Change**
```cpp
// before
auto const rounded = ustl::mm::align_up(size, PAGE_SIZE);
// after
auto const rounded = ustl::round_up(size, PAGE_SIZE);
```

The bodies are unchanged. Not touched:

- The `align_up` helpers that obi's `PackageBuilder`, bootfs's builder, bootmem, multiboot, `phys/build_id_note.cpp` and a few tests define for themselves. They are not ustl's.
- Older change records, which are append-only.

**Reference**
None, this project's own library.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed, which include `launch`'s plan tests, the heaviest caller.
- `git grep "ustl::mm::align_"` outside `docs/` finds nothing.

**Deliberately not done**
- `is_aligned` stays in `ustl::mm`; the user asked for the two `round_*` only.
- Both functions assume the alignment is a power of two (a mask, not a division), and nothing checks it. That check is its own change.

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does `round_down(value, a)` work as `value & ~(a - 1)` only when `a` is a power of two, and what does it give for `a = 3`?
- Why is none of `std::align`, `std::bit_ceil` and `std::is_sufficiently_aligned` a replacement for `round_up` on a byte count?
- `round_up(value, a)` adds `a - 1` before masking; for which values does that wrap, and how do the callers in `VmoDispatcher::create_user()` rule them out?

## ustl::mm::is_aligned is now ustl::is_aligned (2026-10-25)

**Files**
- `system/lib/ustl/include/ustl/mem/align.hpp` — `is_aligned` moves into `ustl` beside `round_up` and `round_down`, and the `ustl::mm` namespace leaves the header
- 10 callers in `kernel/` — the new name; `kernel/arch/x86/kernel/bootstrap.cpp` changes its `using` declaration

**Symptom**
None, a rename the user asked for.

**Cause**
None, a rename. After the previous change one header held two namespaces, `round_*` in `ustl` and `is_aligned` in `ustl::mm`, for three functions that are one family. Nothing else in the tree lives in `ustl::mm`.

**Change**
```cpp
// before
if (!ustl::mm::is_aligned(offset, PAGE_SIZE)) {
// after
if (!ustl::is_aligned(offset, PAGE_SIZE)) {
```

`kernel/arch/x86/kernel/task/thread.cpp` also holds another session's uncommitted rename of `x64ContextSwitchFrame`. This commit takes only its `is_aligned` line: the commit was built in a temporary index from HEAD's copy of that file with the one line changed, so the other session's lines stay uncommitted in the working tree.

**Reference**
None, this project's own library.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required). Host tests 146 passed.
- `git grep "ustl::mm::"` outside `docs/` finds nothing.

**Deliberately not done**
- The power-of-two check on the alignment, the next change.

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does `is_aligned(value, a)` need `a` to be a power of two, and what does it answer for `value = 6`, `a = 3`?
- How can one commit take one hunk of a file whose other hunks belong to someone else, without touching the index the other sessions share?
- Why is the header path `ustl/mem/align.hpp` left as it is although the namespace changed?
