# kernel-task changes

## Title: owned_wait_queue.hpp includes ustl/option.hpp inside its include guard, with the other ustl headers (2026-10-09)

**Files**
- `kernel/include/ours/task/owned_wait_queue.hpp` — `#include "ustl/option.hpp"` moved from above the include guard into the sorted `<ustl/...>` block

**Symptom**
None in behaviour. The header's first line after the licence was `#include "ustl/option.hpp"`, above `#ifndef OURS_TASK_OWNED_WAIT_QUEUE_HPP`.

**Cause**
An include above the guard runs on every inclusion of the header, not once, and the quotes ask the preprocessor to look beside this file first, where no `ustl/` directory exists. It worked only because `ustl/option.hpp` has a guard of its own and the search fell back to the include path. It looks like an editor's automatic include insertion. The edit sat uncommitted in the shared tree from 2026-10-07; another session's run of `scripts/format_cpp.py` sorted it into place on 2026-10-08, and no session claimed it.

**Change**
The include now sits inside the guard as `#include <ustl/option.hpp>`, in sorted order between `<ustl/function/function.hpp>` and `<ustl/traits/is_same.hpp>`.

**Reference**
None, a header hygiene fix.

**Verification**
In a fresh worktree at baseline with only this change: build ok; `boot_check.py` dbg PASS (35 passed), rel PASS (33 passed, 6/6 required), host tests PASS (144).

**Deliberately not done**
The uncommitted edit to `kernel/kernel/task/thread.cpp` that sat beside this one is not committed: it frees a joinable thread that exits before anyone joins it (the failure it causes is described in the 2026-10-09 paragraph of `docs/repairs/261007/kernel-smp.md` and in `docs/repairs/261009/x86-kernel.md` item 1; in that day's investigation interruptline.test failed in 5 of 6 runs with the edit and passed 7 of 7 without it). It is saved as `out/patches/thread-exit-joined-20261009.patch` in the project root.

**How it was found**
The user asked for the unclaimed edits in the shared tree to be checked and committed if sound.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does an include above the include guard run on every inclusion of the header?
- What does `"..."` change about where the preprocessor looks, compared with `<...>`?
- Why did the header still compile before the fix?
