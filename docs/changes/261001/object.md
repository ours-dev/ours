# object changes

## Title: The snapshot options are named for what they freeze, and declared by kind (2026-10-01)

**Files**
- `system/public/ours/syscall.hpp` — `VmoChildOptions::Snapshot` becomes `SnapshotFull` and `SnapshotAtLeastOnWrite` becomes `SnapshotOnWrite`; the enumerators are declared kinds first, modifiers after, with the values unchanged; a comment maps each snapshot to Zircon's syscall name.
- `kernel/object/vmo_dispatcher.cpp`, `examples/shell/shell.cpp` — the uses.

**Symptom**
None; a rename. No value changed, so nothing a program sends is read differently.

**Cause**
The three snapshot kinds are three levels of one guarantee, how much of the parent the child freezes, and the names did not say so: `Snapshot` named none of the three in particular, and `SnapshotAtLeastOnWrite` was long and read backwards. Zircon's kernel already has the better names for the same three, internally, in `SnapshotType { Full, Modified, OnWrite }`; only its syscall header spells them `ZX_VMO_CHILD_SNAPSHOT`, `..._AT_LEAST_ON_WRITE` and `..._MODIFIED`. The user chose names that put the level in the suffix.

The user also asked why the snapshots were not together. They were declared in the order of their values, and the values are Zircon's, handed out one at a time as the kinds were added over the years, so they do not run in order (and bit 1 is not used). The values have to stay Zircon's, so that a request ported from Zircon asks the same question; the order of declaration does not, so the enumerators are now grouped by what they are.

**Change**
```cpp
// The kind of child.
SnapshotFull = 1U << 0,      // ZX_VMO_CHILD_SNAPSHOT
SnapshotOnWrite = 1U << 4,   // ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE
SnapshotModified = 1U << 7,  // ZX_VMO_CHILD_SNAPSHOT_MODIFIED
Slice = 1U << 3,
Reference = 1U << 6,
// What may be added to it.
Resizable = 1U << 2,
NoWrite = 1U << 5,
```

**Reference**
Zircon `SnapshotType` (`zircon/kernel/vm/include/vm/vm_object.h:65`) for the names; `ZX_VMO_CHILD_*` (`zircon/system/public/zircon/types.h:283`) for the values.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts, and the shell's `vmo_create_child` checks passed in both, including the one that asks for `SnapshotFull` and is still answered `NotSupported`.

**Deliberately not done**
- Accepting the snapshots: that is the next commit.

**How it was found**
The user's review of the options, while deciding which of them the clone should accept.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does each of the three snapshot kinds freeze?
- Why can the values not be renumbered so the snapshots are adjacent?
- Where did the new names come from?

## Title: vmo_create_child makes a copy-on-write snapshot, for SnapshotFull and SnapshotOnWrite (2026-10-01)

**Files**
- `kernel/object/vmo_dispatcher.cpp` — `VmoDispatcher::create_child()` accepts `SnapshotFull` or `SnapshotOnWrite`, alone, and answers it with `mm::VmObjectPaged::create_clone()`.
- `system/public/ours/syscall.hpp`, `kernel/lib/syscall/src/vmo.cpp` — comments: which options are built.
- `examples/shell/shell.cpp` — the snapshot checks; the `SnapshotFull` refusal replaced by `SnapshotModified` and two kinds at once.

**Symptom**
A program could not make a copy-on-write child: every snapshot option answered `NotSupported`.

**Cause**
The kernel side was finished (`docs/changes/260929/mm.md`, through `VmObjectPaged::create_clone()`); the syscall's object layer only knew slices.

Both snapshot kinds go to the one clone. It gives a full snapshot (neither side sees the other's later writes), which is everything `SnapshotFull` asks and more than `SnapshotOnWrite` does: the latter guarantees only that a page the child writes is its own, and allows, without requiring, that a page it has not written show the parent's later writes. Zircon answers both the same way for an anonymous VMO, with its bidirectional clone; only a pager-backed parent gets the cheaper unidirectional one, and there is no pager here. The user went through the three kinds and agreed. `SnapshotModified` exists for `fork()` of a mapping of a pager-backed file and stays `NotSupported`.

**Change**
```cpp
auto const snapshot =
    options == VmoChildOptions::SnapshotFull || options == VmoChildOptions::SnapshotOnWrite;
if (options != VmoChildOptions::Slice && !snapshot) {
    return Status::NotSupported;
}
...
if (snapshot) {
    paged->create_clone(offset, rounded, &child);
} else {
    paged->create_child_slice(offset, rounded, &child);
}
```
The size is rounded and checked as for a slice; the offset must be page aligned and no further than the parent's end; the size may run past it, and that part reads as zero. The child handle takes the parent handle's rights, as a slice's does: what a snapshot should get is the next commit.

**Reference**
Zircon `VmObjectDispatcher::CreateChild()` and `sys_vmo_create_child()`; `VmObjectPaged::CreateClone()` with `SnapshotType::Full` and `OnWrite`.

**Verification**
- Every changed C++ line formatted; `clang-tidy` nothing on the changed lines.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts, and the shell's `vmo_create_child checks passed` in both, now with: a `SnapshotFull` child three pages long of a two-page VMO reads the parent's bytes; the child writes its page 0 and the parent still has its own; the parent writes its pages 0 and 1 and the child sees neither; the page past the parent's end reads zero; a `SnapshotOnWrite` child reads the parent as it is then; an unaligned offset is `InvArgs`; `SnapshotModified`, and `SnapshotFull | SnapshotOnWrite`, are `NotSupported`. `vmosnapshot.test` 194 checks on both images.
- The checks can fail: with snapshots answered by a slice, the debug image's shell reported `vmo_create_child checks FAILED`.

**Deliberately not done**
- Rights: the child takes the parent handle's, so a snapshot of the read-only BootFS cannot be written. That is the next commit (Zircon adds Write to a snapshot, and drops Execute).
- `Resizable`, `NoWrite` and `Reference` beside a snapshot stay `NotSupported`.

**How it was found**
None: step 10 of `.agents/tasks/vmo-snapshot-child/DESIGN.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why may `SnapshotOnWrite` be answered with a full snapshot?
- What does the child read past the parent's end, and why is that allowed for a snapshot and not for a slice?
- Why can a snapshot of the BootFS not be written yet?
