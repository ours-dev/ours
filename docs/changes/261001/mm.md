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
