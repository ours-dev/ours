# object changes

## Title: A snapshot child gets Write and loses Execute, as Zircon's does (2026-10-02)

**Files**
- `kernel/lib/syscall/src/vmo.cpp` — `vmo_create_child()`: for `SnapshotFull` and `SnapshotOnWrite`, the child's rights are the parent handle's with `Execute` removed and `Write` added; a slice's stay the parent handle's, unchanged. The doc comment says why for each.
- `examples/shell/shell.cpp` — `check_vmo_create_child()`: a snapshot of a handle without `Write` can be written, and the source keeps its bytes. `check_vma_map()`: a snapshot of an executable VMO cannot be mapped RX, can be mapped RW, and a store through that mapping is not seen through the executable VMO's own RX mapping.

**Symptom**
A snapshot took the parent handle's rights unchanged. A snapshot of a read-only VMO, which is what a program's data segment is cloned from, could not be written, so it was of no use; and a snapshot of an executable VMO kept `Execute`, so a program could write into it and then run it without going through `vmo_replace_as_executable()`.

**Cause**
10a accepted the snapshot options and left the rights as the slice had them. For a slice that is right: a slice writes the parent's own pages, so it must not be able to write more than the parent handle could. A snapshot is different on both counts. A write to it can only ever reach its own copy of a page, never the parent's, so `Write` takes nothing from the parent and is what the snapshot is made for. And being writable, it must not also be executable by default: writable-then-executable code is exactly what `vmo_replace_as_executable()` and the `vmex` resource exist to gate.

**Change**
```cpp
if (options == VmoChildOptions::SnapshotFull || options == VmoChildOptions::SnapshotOnWrite) {
    rights &= ~Rights::Execute;
    rights |= Rights::Write;
}
```

**Reference**
Zircon `sys_vmo_create_child()` (`zircon/kernel/lib/syscalls/vmo.rs:325`): the child's rights are the parent's minus `RESIZE` (plus `GET_PROPERTY`/`SET_PROPERTY`); with `ZX_VMO_CHILD_NO_WRITE` it removes `WRITE`, otherwise, for every snapshot kind, it removes `EXECUTE` and adds `WRITE`. `NoWrite` is not accepted here yet, so only the second branch exists; the property rights are left out as before (design §10).

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed, 0 skipped), rel PASS (13 passed, 10 skipped; the executable-VMO check is skipped there, as rel has no `vmex`), and the shell's `vmo_create_child` and `vma_map` checks passed in both.
- Mutation: the two lines removed. dbg FAIL with `vmo_write(a snapshot of the read-only copy) = AccessDenied`, `vma_map(RX, its snapshot) = Ok` and `vma_map(RW, its snapshot) = AccessDenied`; rel FAIL on the first. Restored.

**Deliberately not done**
- `NoWrite` on a snapshot: still `NotSupported`; nothing here asks for a read-only snapshot.
- A snapshot of the BootFS. It was the first test written and it answers `BadState`: the BootFS VMO is made by `VmoDispatcher::create_paged()`, which makes it Pinned, and `VmObjectPaged::create_clone()` refuses a Pinned VMO (a pinned page must not move into a hidden node or be forked away from its owner; Zircon refuses a clone of a VMO with pinned pages the same way). The checks were moved onto `vmo_create` VMOs, which are lazy. Whether the BootFS has to be Pinned is a decision for the launcher step, which needs to snapshot it.

**How it was found**
Planned as part of step 10; the user approved Zircon's rule on 2026-10-01. The BootFS refusal was found by the first version of this commit's test.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a snapshot be given `Write` that the parent handle lacks, when a slice cannot?
- What would a program be able to do if a snapshot of an executable VMO kept `Execute`?
- Why does a clone refuse a Pinned VMO?
