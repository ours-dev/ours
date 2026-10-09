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

## Title: exit()'s local for a thread whose descriptor is never freed is named never_freed, not thread_desc_recyclable (2026-10-09)

**Files**
- `kernel/kernel/task/thread.cpp` — `Thread::Current::exit()`: the local renamed, a comment on the three ways the descriptor ends, and one comment that named the "recyclable path"

**Symptom**
None in behaviour. The local `thread_desc_recyclable` was true for a Detached thread that is also NoFree, the case where the descriptor is never freed.

**Cause**
"Recyclable" says the descriptor may be taken back and reused. The case is the opposite: a NoFree thread is a CPU's idle thread, whose descriptor does not come from `ThreadCache`, so `exit()` switches away from it for good, queues no Dpc and frees nothing. A reader of the old name would expect the descriptor to go back to the cache, which is exactly what must not happen. The user had just asked what the two locals mean together, and the name was the obstacle.

**Change**
`thread_desc_recyclable` became `never_freed`. Beside it, a comment now lists the three ways a descriptor ends: never freed (Detached and NoFree), freed by its reaper (joinable), or freed by the Dpc (Detached). The two locals are never true together: one is set only on the Detached branch, the other only off it.

**Reference**
None, a rename.

**Verification**
- `git grep thread_desc_recyclable` finds nothing.
- `boot_check.py`: build ok; dbg PASS (35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 6/6 required), host tests PASS (144).
- `kshell_run.py --out out/bringup.x86 threadjoin.test`: ok, 11 checks.

**Deliberately not done**
None.

**How it was found**
The user asked what the combination of `joinable` and `thread_desc_recyclable` means, and agreed to the rename.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which threads are NoFree, and why must their descriptors never go back to ThreadCache?
- Why can `never_freed` and `joinable` never both be true?
- Why does a joinable thread's Dpc signal `exited()` instead of freeing the thread?

## Title: exit() decides how the descriptor ends as one Ending value, Kept, SelfFreed or Zombie, not two bools (2026-10-09)

**Files**
- `kernel/kernel/task/thread.cpp` — `Thread::Current::exit()`: the locals `never_freed` and `joinable` replaced by a function-local `enum class Ending : u8 { Kept, SelfFreed, Zombie }`; the comments that named the old locals now name the enumerators

**Symptom**
None in behaviour. Two bools described one three-way choice, so a reader had to work out that `never_freed && joinable` cannot happen, and which pair meant "the Dpc frees it".

**Cause**
The descriptor ends in exactly one of three ways, and the two bools could spell four. Kept: Detached and NoFree, a CPU's idle thread, whose descriptor is not from `ThreadCache`; it switches away for good and nothing frees it. SelfFreed: Detached; `exit()` unlinks it from `SPAWNED_THREADS` and the Dpc destroys it once it is off its stack. Zombie: not Detached, whether or not a `join()` has claimed it (`Flags::Joined`); it stays on `SPAWNED_THREADS`, and the Dpc only signals `exited()`, after which its reaper, `join()` or a later `detach()`, unlinks and destroys it.

**Change**
One `Ending` value starts as `Zombie`, becomes `SelfFreed` on the Detached branch, and `Kept` when NoFree is also set; all three are decided under the thread's PI lock, as before. `Kept` switches away inside that lock scope and never returns. The other two pick the Dpc function: `destroy()` for `SelfFreed`, the `exited()` signal for `Zombie`. The enum is a plain scoped enum, not a bitmask: its values are exclusive, and a bitmask operator could not be found for a function-local enum anyway (a block-scope declaration is not reached by argument-dependent lookup, so `USTL_ENABLE_ENUM_BITMASK` does not apply there).

**Reference**
The three outcomes follow the thread-reaping states in `kernel/include/ours/task/thread.hpp` (Detached, Joined, neither) and the reaping design in `.agents/tasks/thread-join/DESIGN.md`.

**Verification**
- `boot_check.py`: build ok; dbg PASS (35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 8 skipped, 6/6 required), host tests PASS (144).
- `kshell_run.py --out out/bringup.x86 threadjoin.test`: ok, 11 checks.
- `kshell_run.py --out out/bringup.x86 interruptline.test`: ok, 58 checks, in 3 of 3 runs. This is the test that failed in 5 of 6 runs when a joinable, unclaimed thread was freed by its own Dpc.

**Deliberately not done**
The user's own rewrite of this block, which sat uncommitted in the shared tree, is not applied; it is saved as `out/patches/thread-exit-user-operation-20261009.patch` in the project root. It did not compile (a bitmask on a function-local enum, and `func` used where it was not declared), and its logic had four faults the review raised: it signalled `exited()` before the thread was off its stack, so a joiner could free a descriptor the thread still ran on; a joinable thread could be freed both by itself and by its reaper; a Detached thread was not unlinked from `SPAWNED_THREADS`; and the closing `panic()` became `unreachable()`, which turns a scheduler bug into undefined behaviour instead of a message.

**How it was found**
The user set out to collapse the states in `exit()`, wrote a version, and asked for it to be reviewed and for a correct one; this is that version, applied at the user's request.

**The user's prediction**
The user's model before the review: a Detached thread frees itself; a Joined thread is freed by its joiner; otherwise the thread frees itself. The first two hold. The third does not: a thread that is joinable but not yet joined must stay a zombie, because a `join()` or `detach()` may still come for it, and freeing it then is a use-after-free. That is why `Zombie` covers Joined and joinable alike.

**Three questions you should be able to answer**
- Why must a joinable thread that nobody has joined yet stay on `SPAWNED_THREADS` after it exits?
- Why is `exited()` signalled from a Dpc and not from `exit()` itself, just before the reschedule?
- Why must the `Zombie` Dpc touch nothing of the thread after it signals `exited()`?
