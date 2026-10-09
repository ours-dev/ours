# object changes

## VmoChildOptions becomes a bitmask, is_snapshot() reads any snapshot bit, and create_child() checks for one kind itself (2026-10-09)

**Files**
- `system/public/ours/syscall.hpp` — the user's change: `VmoChildOptions` numbered contiguously (the kinds on bits 0 to 4, Resizable on 5, NoWrite on 6) and given bit operators with `USTL_ENABLE_ENUM_BITMASK`.
- `kernel/object/include/ours/object/vmo_dispatcher.hpp` — the user's change: `is_snapshot()` true when any snapshot bit is set; its comment now says what is beside the bit is `create_child()`'s to judge.
- `kernel/object/vmo_dispatcher.cpp` — `create_child()` refuses anything but exactly one kind alone, with NotSupported, before it asks `is_snapshot()`.
- `system/app/shell/shell_tests.cpp` — `check_vmo_create_child()`: `SnapshotFull | NoWrite` and `SnapshotFull | Slice` are NotSupported.

**Symptom**
With the user's `is_snapshot()` alone, both images failed `check_vmo_create_child()`: `shell: vmo_create_child(SnapshotFull | SnapshotOnWrite) = Ok (expected NotSupported)`. Three other sessions saw the same failure in the shared tree from 2026-10-07 and worked around it in clean worktrees. Two further combinations went through unseen, since nothing checked them: `SnapshotFull | NoWrite` came back Ok, and with a handle that has Write, and `SnapshotFull | Slice` came back Ok, as a copy.

**Cause**
`is_snapshot()` answered two questions at once. Its old form, `options == SnapshotFull || options == SnapshotOnWrite || options == SnapshotModified`, said both "this is a snapshot" and "nothing else is beside it", and `create_child()` relied on the second: its only refusal of a combination was `options != Slice && !is_snapshot(options)`. The user's form, `!!(options & MASK)`, answers only the first, which is the right meaning for the predicate: `vmo_create_child()` uses it to decide the child's rights, and a snapshot that one day carries a modifier must still get a snapshot's rights. But once the predicate stopped checking "alone", nothing did, so every combination holding a snapshot bit reached `create_clone()`:
- Two snapshot kinds at once: the kernel built one of two different meanings without saying which.
- `SnapshotFull | Slice`: the caller asked for a window that shares the parent's pages and was given a copy, so its writes and the parent's never meet.
- `SnapshotFull | NoWrite`: the caller asked for a child it cannot write, and `vmo_create_child()`, which gives every snapshot Write, handed back a writable one. A request to drop a right came back with the right added.

**Change**
Validation moved into `create_child()`, where the contract lives (its comment, and `syscalls.gni`: "one of Slice, SnapshotFull, SnapshotOnWrite and SnapshotModified, alone"); `is_snapshot()` keeps the user's meaning.

Before:

```cpp
auto const snapshot = is_snapshot(options);
if (options != VmoChildOptions::Slice && !snapshot) {
    return Status::NotSupported;
}
```

After:

```cpp
constexpr auto KINDS = VmoChildOptions::SnapshotFull | VmoChildOptions::SnapshotOnWrite |
                       VmoChildOptions::SnapshotModified | VmoChildOptions::Slice |
                       VmoChildOptions::Reference;
auto const kind = options & KINDS;
if (options != kind || !ustl::has_single_bit(std::to_underlying(kind))) {
    return Status::NotSupported;
}
auto const snapshot = is_snapshot(kind);
if (kind != VmoChildOptions::Slice && !snapshot) {
    return Status::NotSupported;
}
```

`options != kind` refuses a modifier beside the kind, `has_single_bit` refuses no kind and two kinds. A combination stays NotSupported rather than InvArgs, as the contract already said: Resizable and NoWrite are deferred, not wrong. Rejected: putting "alone" back into `is_snapshot()`, which would make the predicate wrong for the rights decision the day a modifier is built.

The renumbering departs from the values `docs/changes/260928/mm.md` recorded as copied from `ZX_VMO_CHILD_*`; it is the user's decision, and nothing in the tree spells the values out.

**Reference**
Zircon `VmObjectDispatcher::CreateChild()` (`zircon/kernel/object/vm_object_dispatcher.cc`) takes the one child-type bit off the options and refuses any combination of types, and whatever is left, with ZX_ERR_INVALID_ARGS; `sys_vmo_create_child()` takes ZX_VMO_CHILD_NO_WRITE off first and drops ZX_RIGHT_WRITE for it, which is the order to follow when NoWrite is built here.

**Verification**
- clang-format clean on the changed lines; clang-tidy (`out/bringup.x86`) reports nothing on them.
- In `.worktrees/wip-proc-self` at baseline `b5e4e4a1` plus only these four files (the shared tree also holds other sessions' unfinished work): build ok, `dbg PASS 35 passed, 3 skipped, 0 failed`, `rel PASS 33 passed, 9 skipped, 0 failed, 6/6 required`, host tests 144 passed. Both images log NotSupported for `SnapshotFull | SnapshotOnWrite`, `Slice | NoWrite`, `SnapshotFull | NoWrite` and `SnapshotFull | Slice`.
- With `create_child()`'s new check taken out (the user's `is_snapshot()` kept): `rel FAIL 1 failed`, with `SnapshotFull | SnapshotOnWrite`, `SnapshotFull | NoWrite` and `SnapshotFull | Slice` each `= Ok (expected NotSupported)`. So the two new checks guard this change.

**Deliberately not done**
- NoWrite and Resizable are still NotSupported. Building NoWrite means taking Write off in `vmo_create_child()`'s rights step, not adding it; that is a feature for when something asks for it.
- The older checks in `check_vmo_create_child()` still build their combinations with `static_cast` over `std::to_underlying`; only the new lines use the bit operators.

**How it was found**
The sampler stress test's first `ours-verify` in the shared tree failed `vmo_create_child` on both images while a clean worktree passed. Grepping the shell log for `(expected` gave the one failing line; `git diff` of the shared tree showed `is_snapshot()`'s new mask form; reading `create_child()` showed it was the only refusal of a combination. Asking the live sessions (`-7f`, `-f8`, `-57`) found no owner, and the user confirmed the edit was theirs.

**The user's prediction**
The user held that `is_snapshot()` should mean "a snapshot bit is set", and that is what it now means. What was missing was elsewhere: `create_child()` had been borrowing the old exact-match form as its validation.

**Three questions you should be able to answer**
1. Why was `SnapshotFull | NoWrite` returning Ok worse than a wrong status code? What did the caller end up holding?
2. `is_snapshot()` and `create_child()` now ask different questions of the same bits. What is each question, and why does the rights decision in `vmo_create_child()` need the first and not the second?
3. Why is two kinds at once NotSupported here and InvArgs in Zircon, and what in the contract decides which?
