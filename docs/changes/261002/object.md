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

## Title: The BootFS is committed, not pinned, so a file in it can be snapshotted (2026-10-02)

**Files**
- `kernel/object/include/ours/object/vmo_dispatcher.hpp`, `kernel/object/vmo_dispatcher.cpp` — `VmoDispatcher::create_committed()`: anonymous memory with every page committed and zeroed when it is made, neither `Lazy` nor `Pinned`.
- `kernel/lib/userabi/src/user_bootfs.cpp` — `make_user_bootfs()` makes the BootFS VMO with it instead of `create_paged()`.
- `examples/shell/shell.cpp` — `check_vmo_create_child()`: a `SnapshotFull` of the `vdso` entry of the BootFS can be written, and the BootFS still reads the ELF magic.

**Symptom**
`vmo_create_child(bootfs, SnapshotFull, ...)` answered `BadState`, so a program's data segment could not be made as a snapshot of its file, which is what the launcher step needs.

**Cause**
The BootFS VMO was made by `create_paged()`, which makes it `Pinned`. `VmObjectPaged::create_clone()` refuses a Pinned VMO, rightly: a clone moves the original's pages into a hidden node, and a later write by the original forks its page, so the frame under a pinned offset would change. Zircon refuses the same clone (`ZX_ERR_BAD_STATE` for a VMO with pinned pages). The BootFS never needed the pin: it is data the kernel writes before user space starts, and nothing reaches it by physical address. Only the commit was wanted, so that the gaps between files read as zero; `create_paged()` gave both because it was the only factory that committed.

**Change**
```cpp
// Neither Lazy nor Pinned: VmObjectPaged::create() commits every page of a
// VMO that is not Lazy, and pins them only for Pinned.
mm::VmObjectPaged::create(mm::GAF_USER, size, mm::VmoFlags{}, &vmo);
```

**Reference**
Zircon's BootFS VMO is an ordinary paged VMO, never pinned: userboot decompresses the ZBI's BootFS into a VMO it makes with `zx_vmo_create`, and writes every page of it.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed), rel PASS (13 passed, 10 skipped; rel has no BootFS, so the new check runs in dbg only).
- Mutation: the BootFS made by `create_paged()` again. dbg FAIL with `vmo_create_child(bootfs, SnapshotFull, vdso) = BadState (expected Ok)`. Restored.

**Deliberately not done**
- `create_paged()` itself, still Pinned for userboot's stack and the ELF images kelfload loads. Its comment says the pin is there because nothing could service a fault yet, which is no longer true for lazy VMOs; whether those two still need it is its own question.

**How it was found**
The first version of the previous change's test snapshotted the BootFS and was answered `BadState`. The user chose on 2026-10-02 to make the BootFS unpinned rather than allow a clone of a pinned VMO.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What would go wrong if a clone of a pinned VMO were allowed?
- What did the BootFS need from `create_paged()`, and what did it get that it did not need?
- Why does the new check run only in the dbg image?
