# launch changes

## Title: A writable segment is mapped from a copy-on-write snapshot of the file, and its .bss from zero-fill memory (2026-10-02)

**Files**
- `system/lib/launch/include/launch/plan.hpp` — `Source::Private` becomes `Source::Snapshot` and `Source::Zero`; a `Snapshot` record per segment (`snapshot()`) and a zero-fill size (`zero_size()`) replace `private_size()`; `Copy` now only carries the file bytes of the page .bss starts in; `add_writable_mapping()` makes one mapping of a writable segment; `Relro::size` is documented as p_memsz; the `MAX_MAPPINGS` comment gives the real reason for three.
- `system/lib/launch/src/plan.cpp` — the writable branch of `Plan::make()` and its specification.
- `system/lib/launch/test/plan_test.cpp` — nine tests updated, eight new: 36 in all.
- `system/lib/launch/README.md` — describes `Plan::build()` and the three sources.

**Symptom**
None that runs: nothing executes a `Plan` yet. As specified, every writable segment was copied whole into a private VMO at every launch, so no page of a program's data was ever shared between two processes running it, and a launch cost a copy of all of its data.

**Cause**
The explicit `Copy` was chosen on 2026-09-29 because the kernel answered `NotSupported` for every child but a slice. It now builds copy-on-write snapshots (`.agents/tasks/vmo-snapshot-child`, steps 1-11), which is what this was waiting for.

A writable segment's pages are of three kinds, and the plan now treats each as what it is:
1. Pages that hold file bytes only. They come from a snapshot of the file (`vmo_create_child`, `SnapshotOnWrite`): shared by every process until one writes a page, which then becomes its own copy. Relocations written into them before mapping copy just the pages they touch.
2. The page the file bytes end in, when .bss follows them there. Its bytes after the data must read as zero, but in the file they are whatever follows the segment, so it cannot come from the snapshot.
3. The pages of .bss after it, which are zero.

Kinds 2 and 3 are one zero-fill VMO, and one `Copy` writes the few file bytes of kind 2 into its start. A segment without .bss (`mem_size == file_size`) is all kind 1, even if its data ends mid-page: the bytes past the data are past `mem_size` too, and nothing promises what they hold.

The partial page could also have been avoided at link time, by padding the data to a page boundary so that the file itself has the zeros. The user chose to handle it at run time, so that an ELF file linked elsewhere still loads. Of the two run-time ways, Zircon's local loader copies the partial bytes into zero-fill memory (`kCopyInProcess`) and its remote loader snapshots the page and zeroes its tail (`kZeroInVmo`, with `ZX_VMO_OP_ZERO`). This copies, which here writes less than a page into a fresh zero page; zeroing would first copy the whole page and then write zeros over most of it, and ours has no zeroing operation on a VMO.

The bytes of the first page before `vaddr` are not the segment's, and ELF promises nothing about them. When that page comes from the snapshot they now show the file's bytes there (they used to read as zero, from the private VMO), as the file's own pages do for a read-only segment. When it is the zero-fill page -- the segment starts in the page its data ends in, which is the case for every writable segment in the tree today -- the Copy starts at `vaddr` and they still read as zero. Zircon differs there: it rounds the segment's offset down to the page first (`load.h:342-359`) and copies from the start of the page, so it shows the file's bytes. The difference is harmless and recorded in the specification.

`Relro::size` has to be PT_GNU_RELRO's p_memsz. LLD 18 and later end RELRO with `.relro_padding`, a section with no bytes in the file that rounds RELRO up to a page; every program in the tree has it (the shell: p_filesz 0x468, p_memsz 0x6c0). With p_filesz the last RELRO page would be left writable. The comment now says so, and a test uses the shell's real numbers.

**Change**
```cpp
auto const data_end = segment.vaddr + segment.file_size;
auto zero_from = end;
if (segment.mem_size != segment.file_size) {
    zero_from = ustl::mm::align_down(data_end, PAGE_SIZE);
}
// [first, zero_from): Snapshot{align_down(file_offset), zero_from - first}
// [zero_from, end):   a zero-fill VMO, and one Copy of the file bytes
//                     from max(vaddr, zero_from) to data_end
// mappings: [first, end) cut at zero_from and at the end of RELRO
```
A writable segment now gives one to three mappings; the three appear when the RELRO reaches past the snapshot into the zero-fill part.

**Reference**
Zircon `elfldltl::VmarLoader::Load()` (`src/lib/elfldltl/include/lib/elfldltl/vmar-loader.h:274-310`): file pages, then the intersecting page and the zero-fill pages, with `PartialPagePolicy` choosing between `kCopyInProcess` (`LocalVmarLoader`), `kZeroInVmo` (`RemoteVmarLoader`) and `kProhibited` (`AlignedRemoteVmarLoader`, after `SegmentWithVmo::AlignSegments()` has zeroed the partial page once per file).

**Verification**
- Host tests: `prebuilt/ninja/ninja -C out/phys-host-tests host/obj/system/lib/launch/test/plan`, then the binary: 36 of 36 pass (28 before, 8 new: no .bss, data ending on a page boundary, data over several pages ending mid-page, data starting and ending in one page with .bss, RELRO reaching into the zero-fill part, RELRO ending before it, the shell's real RELRO segment by p_memsz and by p_filesz, a snapshot ending in the file's last partial page).
- An adversarial review in three lenses, each re-checked by a skeptic: the code matches its specification exactly, and every finding was about comments or this record (all fixed). One reviewer ran `plan.cpp` under `-fsanitize=unsigned-integer-overflow,undefined` over 563,997 accepted and 1,169 refused plans with no trap; another simulated a launcher carrying out 300,000 generated layouts and the real LLD 23 binaries of the shell, init and fault_probe, and every one gave the ELF's memory image.
- `clang-tidy -p out/phys-host-tests` on `plan.cpp` and `plan_test.cpp`: no diagnostics in the project's files.
- `boot_check.py`: both images boot; nothing in them uses the planner.

**Deliberately not done**
- The executor that carries out a `Plan`, as before. It has to write a relocation that straddles the cut in two parts, one into each VMO; the cut is `first + snapshot(i).size`.
- `docs/design/260923/userspace-interface.md` 3.2 still describes a writable segment as `vmo_create` plus `memcpy`; it is an accepted design and is not rewritten.
- Zircon's per-file preparation (`AlignSegments`): zeroing the partial page once in a cached snapshot so that every later launch shares that page too. An optimisation for when a program is launched often.
- Page alignment at link time, which the user declined.

**How it was found**
None; step 12 of `.agents/tasks/vmo-snapshot-child`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can the page the file bytes end in not come from the snapshot when .bss follows them, but can when there is no .bss?
- Why copy the partial page's bytes into zero-fill memory rather than snapshot that page and zero its tail?
- When does a writable segment give three mappings, and from which VMO is each?
