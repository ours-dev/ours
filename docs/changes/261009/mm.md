# mm changes

## VmObjectPaged::create_child_slice() is renamed create_slice(), the pair of create_clone() (2026-10-09)

**Files**
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`, `kernel/mm/vm_object_paged.cpp` — the declaration, the definition and the sibling note on `create_clone()`.
- `kernel/object/vmo_dispatcher.cpp` — the call in `create_child()` and its comment.
- `system/public/ours/syscall.hpp` — `VmoChildOptions::Slice`'s "Built:" note.
- `kernel/commands/vmo_snapshot_test.cpp` — four calls.

**Symptom**
None, a rename. The two ways `VmObjectPaged` makes a child were named unevenly: `create_child_slice()` and `create_clone()`.

**Cause**
The names came from two places: `create_child_slice()` from Zircon's `CreateChildSlice()`, `create_clone()` from its `CreateClone()`, which are uneven there too. Every child-making function here returns a new child, so "child" told the reader nothing that `create_` did not, and its presence on one of the two made them read as two different kinds of operation. The user chose `create_slice()` to match `create_clone()`.

**Change**
`create_child_slice` became `create_slice` everywhere in code and comments; clang-format reflowed the one statement in `vmo_snapshot_test.cpp` whose length changed. The change records and task documents that mention the old name are history and keep it.

**Reference**
Zircon `VmObject::CreateChildSlice()` and `VmObject::CreateClone()`, the names this pair departs from.

**Verification**
In `.worktrees/wip-proc-self` at baseline `6792f432` plus only this rename: build ok, `dbg PASS 35 passed, 3 skipped, 0 failed`, `rel PASS 33 passed, 9 skipped, 0 failed, 6/6 required`, host tests 144 passed; `kshell_run.py vmosnapshot.test`: `ok -- 205 checks`. `git grep create_child_slice -- kernel system` finds nothing.

**Deliberately not done**
`create_clone()` was not renamed `create_snapshot()` to match the ABI's `Snapshot*` kinds; the user evened the pair by renaming the slice instead.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. What does a slice share with its parent that a clone does not, and what does that mean for a write through either?
2. Why does every function that makes a `VmObjectPaged` start with `create_` rather than being a plain verb like `clone()`?
3. Which documents still spell the old name, and why are they left as they are?
