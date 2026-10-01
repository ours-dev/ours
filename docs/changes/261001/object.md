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
