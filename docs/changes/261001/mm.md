# mm changes

## Title: VmMapOption::Commit is renamed Populate, which is what it does (2026-10-01)

**Files**
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` — the enumerator, and a comment on what it means.
- `kernel/mm/vm_area.cpp`, `kernel/mm/include/ours/mm/vm_area.hpp`, `kernel/mm/stack.cpp`, `kernel/object/vma_dispatcher.cpp`, `kernel/lib/heap/src/vmalloc.cpp`, `kernel/arch/x86/apic/local_apic.cpp`, `kernel/arch/x86/apic/io_apic.cpp`, `kernel/arch/x86/kernel/descriptor.cpp`, `kernel/arch/x86/kernel/idt.cpp`, `kernel/arch/x86/kernel/time/hpet.cpp`, `kernel/commands/vmo_snapshot_test.cpp` — the uses.

**Symptom**
None; a rename. It prepares the next change, which makes the option do only what the new name says.

**Cause**
The option asks for the page tables of a mapping to be filled in when the mapping is made, which is what Linux calls `MAP_POPULATE` and Zircon `ZX_VM_MAP_RANGE`; the user-facing flag that becomes it is `VmOptions::MapRange`. Calling it `Commit` named it after a different operation, `VmObject::commit_range()`, which asks that every page of the object be its own, and the implementation followed the name: `VmArea::map_with_vmo()` commits the object before it maps it. Without copy-on-write the two gave the same pages, so the confusion cost nothing; with it, commit copies every shared page, and populate should not.

**Change**
`VmMapOption::Commit` becomes `VmMapOption::Populate` at its definition and its eleven uses, with a comment on the enumerator saying what it is and what it is not. No behaviour changes.

**Reference**
Zircon `ZX_VM_MAP_RANGE`, which the syscall layer turns into `VmMapping::MapRange(..., /*commit=*/false, ...)` (`zircon/kernel/lib/syscalls/vmar.cc:193`); Linux `MAP_POPULATE`.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts; `vmosnapshot.test` 194 checks ok on both images.

**Deliberately not done**
- The behaviour: `map_with_vmo()` still commits before it maps. That is the next commit.

**How it was found**
The user chose the name, after asking what populate and commit each mean.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does populate promise, and what does commit promise, on a copy-on-write clone?
- Why did the two names coincide in behaviour until clones existed?
- Which user-facing flag becomes this option?

## Title: Populate maps what the object has, and commits nothing (2026-10-01)

**Files**
- `kernel/mm/vm_area.cpp` — `VmArea::map_with_vmo()` no longer calls `commit_range()` for `VmMapOption::Populate`.
- `kernel/commands/vmo_snapshot_test.cpp` — the populated-clone checks go through `map_with_vmo()` with `Populate`, the path a program's `VmOptions::MapRange` takes.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — the open question closed, step 9 done.

**Symptom**
A program that mapped a copy-on-write clone with `MapRange` would have had every shared page of the window copied when the mapping was made, and mapped writable: the copying the clone exists to put off, all at once. Latent: nothing makes a clone that a program can map yet.

**Cause**
`map_with_vmo()` implemented populate as "commit the object, then map it". Commit is the object's promise that every page is its own (`commit_range()` asks the cursor for an owned page, as Zircon's `CommitRangeLocked()` asks `RequireOwnedPage()`); populate is only a promise that the page tables are filled in. For an object with no ancestors the two give the same pages, so the shortcut went unnoticed. For a clone, owning a page means copying it. The user decided on 2026-10-01 that populate does what its name says, as Zircon's `ZX_VM_MAP_RANGE` does (`MapRange(..., /*commit=*/false, ...)`), and `commit_range()` keeps its meaning for whoever asks for it explicitly.

**Change**
```cpp
if (!!(VmMapOption::Populate & option)) {
    status = mapping->map(mapping->base(), size, true, MapOptions::ErrorIfExisting);
```
without the `commit_range()` before it. `map()` maps what the cursor answers a read with (the previous entry of `docs/changes/260929/mm.md`).

The kernel's own populated mappings are unchanged in what they get: a stack and vmalloc's memory are made from `Pinned` objects, which are committed and pinned when they are created, and the APIC, IO-APIC, HPET, descriptor table and IDT map physical objects, which have no pages to commit. For an object with no ancestors a read of a hole in `map_paged()` is a zeroed page of the object's own, as the commit gave.

**Reference**
Zircon `ZX_VM_MAP_RANGE` in `sys_vmar_map()` (`zircon/kernel/lib/syscalls/vmar.cc:193`) and `VmAddressRegion` (`vm_address_region.cc:788` and after), all `MapRange(..., /*commit=*/false, ...)`; `VmCowPages::CommitRangeLocked()` (`vm_cow_pages.cc:4004`) for what commit is.

**Verification**
- `scripts/format_cpp.py --check` clean.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts, so the kernel's populated mappings (stacks, vmalloc, the APICs, HPET, GDT, IDT) still come up. `vmosnapshot.test` 194 checks ok on both images, now with the clone populated through `map_with_vmo()`; `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel.
- The checks can fail: with the `commit_range()` put back, three of 194 fail (the shared page on the object's frame, the zero page, and nothing copied).

**Deliberately not done**
- No explicit commit syscall; `commit_range()` is reached from the kernel only.

**How it was found**
A first version of the populated-clone test, which found the page tables full of copies; the user chose the fix and the name after asking what populate and commit each mean.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What did populate cost on a clone before this, and why only on a clone?
- Why do the kernel's own populated mappings not need the commit?
- Where does a program's `MapRange` become `VmMapOption::Populate`?
