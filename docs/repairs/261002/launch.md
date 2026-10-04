# launch outstanding repairs

## 1. The vDSO's exports are read from a file that nothing checks against the vDSO that is mapped

**Severity**
Medium: wrong under a condition no caller meets today.

**Symptom**
Latent, cannot be triggered today. A caller that passes `launch::Request::vdso_file` from another build, or from a vDSO variant with the same number of pages, gets a process whose imports point at the wrong offsets inside the vDSO. The vDSO is the one range the kernel accepts system calls from, so the program then jumps to the wrong place inside trusted code.

**Cause**
`launch::start()` maps `request.vdso`, the kernel's vDSO VMO, but takes the span and the exports it binds against from `request.vdso_file` (`system/lib/launch/src/launch.cpp`, steps 1 and 4). The only check that the two belong together is the kernel's test that the mapping covers the whole VMO (`kernel/object/vma_dispatcher.cpp:127`), which any file of the same page count passes. Today's one caller, the shell, reads `vdso` out of the BootFS it was handed, the file the kernel laid the VMO out from (`kernel/lib/userabi/src/userboot.cpp:43`), so the two agree by construction. That is why the interface design's D13 dropped the build-id check (revised 2026-10-02).

**Change**
Bind against the bytes of the VMO that is mapped, as Zircon does: the VmoVdso handle grants Read (`kernel/lib/userabi/src/userboot.cpp:431`). The VMO holds the vDSO laid out by address, not as a file (`kelfload::Loader::load()`), so this needs either a binder that reads a loaded image, or a check: compare the ELF header, program headers and build-id note in the VMO's first page with the file's, and refuse with InvArgs on a mismatch.

**Reference**
Zircon's process_builder decodes the vDSO from the VMO it maps, `Elf64Headers::from_vmo(&vdso)` (`src/lib/process_builder/src/process_builder.rs:619-620`), as Fuchsia's userboot does (`src/bringup/lib/userboot/rust/src/program.rs:396-397`).

**Acceptance**
A launch handed a `vdso_file` that differs from the VMO is refused with InvArgs and a report saying so; the shell's launcher checks still pass on dbg.

## 3. A launch that snapshots a program makes the whole BootFS pay for it

**Severity**
Low: a performance cost, and a departure from Zircon's semantics; nothing wrong.

**Symptom**
Not measured. A launch whose plan makes a snapshot does work in proportion to the whole BootFS, not to the program. Today that is the shell's launch of the control probe: fault_probe's table of relocated pointers is the one writable segment in the tree with whole pages of file bytes.

**Cause**
The shell's file VMO is a Slice of the executable BootFS. `start()` makes a SnapshotOnWrite child of that slice for each writable segment with whole pages of file bytes before its cut. `VmObjectPaged` clones the slice through the BootFS's own `VmCowPages` at the slice's offset (`kernel/mm/vm_object_paged.cpp:161-163`). So the hidden node goes above the whole BootFS. Making it moves no pages, because `take_all_from()` swaps the two trees (`kernel/mm/include/ours/mm/vm_page_map.hpp:117-126`). Two things do cost:
- When no snapshot of the BootFS is alive, the clone makes a new hidden node, and `remove_write_locked(0, m_size)` runs over every mapping of the BootFS node (`kernel/mm/vm_cow_pages.cpp:387`). Every launched program's code is one of those mappings. A clone made while another is alive hangs under the existing hidden node and pays neither cost (`kernel/mm/vm_cow_pages.cpp:332-354`).
- When the last snapshot goes, the hidden node has one child left and is merged back into it page by page (`merge_into_only_child_locked()`, `kernel/mm/vm_cow_pages.cpp:139-211`). That is O(the BootFS's pages).

Zircon refuses this clone outright. A clone through a slice of an anonymous VMO must be unidirectional (`zircon/kernel/vm/vm_object_paged.cc:771-776`), and one that cannot be answers ZX_ERR_NOT_SUPPORTED (`vm_cow_pages.cc:1891-1893`, `:1911-1913`). Fuchsia's userboot instead makes each program file a snapshot of the BootFS once, then makes that snapshot executable (`src/bringup/lib/userboot/rust/src/program.rs:233-243`).

**Change**
Measure first, with the user (CLAUDE.md, performance work): the time of the control probe's launch against the BootFS's size and the number of programs running from it. Candidates:
- Fuchsia's shape: one snapshot per file, made executable with vmo_replace_as_executable and kept for later launches. The hidden node is then made once per file. This also stops the shell holding an Execute handle to the whole BootFS (the filesystem design's D3).
- Refuse a snapshot through a slice, as Zircon does. That forces the first option on every caller.

**Reference**
Fuchsia userboot `create_bootfs_file_vmo()` (`src/bringup/lib/userboot/rust/src/program.rs:233-243`); Zircon `VmObjectPaged::CreateClone()` (`zircon/kernel/vm/vm_object_paged.cc:771-776`).

**Acceptance**
The measurement above, before and after, agreed with the user.

## 4. Every relocation is a system call

**Severity**
Low: a performance cost, nothing wrong.

**Symptom**
Not measured. A launch makes one `vmo_write` per relocation, and two for one that straddles a cut. fault_probe, the program the launcher starts, has 1142 (20 R_X86_64_GLOB_DAT and 1122 R_X86_64_RELATIVE, 1100 of them its table of relocated pointers), so each launch of it makes 1142 system calls. The shell has 106.

**Cause**
The binder hands each 8-byte relocation to the Writer separately (`system/lib/elfload/src/binder.cpp`, `store()`). `Writer::write()` (`system/lib/launch/src/launch.cpp`) searches the plan's mappings and then makes one `ou::vmo_write` per piece. `vmo_write` was chosen when the launcher mapped nothing into itself and the kernel had no `vma_unmap` (the filesystem design, §4.6). Neither holds since 2026-10-04: `vma_unmap` exists (component-framework step 9), and `start()` maps the program's whole file into the caller for as long as it reads it, then unmaps it (step 11, design D14). Only the writable VMOs are still written by system call, so the rule that made them so is gone; what is left is the trade the Change below measures.

**Change**
Measure first, with the user: the number of `vmo_write` calls and the launch latency of fault_probe, whose table makes the count large. Candidates:
- Batch neighbouring relocations into one `vmo_write` per page or per run.
- Map each writable VMO, snapshot or zero-fill, into the launcher once and write in memory, as Zircon's remote linker does. `vma_unmap` exists now, and `start()` already maps and unmaps the file once per launch; this adds one map and one unmap, with its TLB shootdown, per writable VMO, against one system call per relocation.

**Reference**
Zircon `ld::RemoteLoadModule::Relocate()` (`sdk/lib/ld/include/lib/ld/remote-load-module.h:304-327`) writes through `elfldltl::SegmentWithVmo`'s `GetMutableMemory()` (`src/lib/elfldltl/include/lib/elfldltl/segment-with-vmo.h:323-360`), which maps each mutable VMO once.

**Acceptance**
The measurement above, before and after, agreed with the user.
