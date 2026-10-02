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

## Title: The launcher reads a program's segments, RELRO and entry point out of its ELF (2026-10-02)

**Files**
- `system/lib/launch/include/launch/program.hpp`, `system/lib/launch/src/program.cpp` — `launch::Program::read()`: the PT_LOAD segments in file order, the one PT_GNU_RELRO and `e_entry`, out of the file's ELF and program headers, through eftl's parser.
- `system/lib/launch/BUILD.gn`, `system/lib/launch/test/BUILD.gn` — the source, eftl as a dependency, and a second host test, `program`.
- `system/lib/launch/test/program_test.cpp` — seven tests on ELF images built byte by byte.
- `.agents/tasks/system-abi-surface/DESIGN.md` — the user's choice to write the K23 executor against today's `ou`.

**Symptom**
None; the first step of K23's executor. `Plan::build()` takes segments and a RELRO as numbers, and nothing turned a program file into those numbers.

**Cause**
An ELF file says how it wants to be loaded in its program headers, one entry each:
- PT_LOAD: a range of the file (`p_offset`, `p_filesz`) to place at an address (`p_vaddr`), `p_memsz` long in memory (the rest is .bss), with `p_flags` for read, write and execute.
- PT_GNU_RELRO: a range that becomes read-only once relocations are written. Its size is `p_memsz`, because LLD's `.relro_padding` rounds RELRO up to a page without taking room in the file (the previous entry).
- Everything else (PT_DYNAMIC, PT_GNU_STACK, notes) is not the planner's business; the binder reads PT_DYNAMIC from the file itself later.

`read()` only takes the numbers out; it does not decide whether the file is a program this system can start. That question already has one answer, `elfload::check_program()`, which the kernel's loader asks and which the executor will ask before it reads, so that the launcher refuses exactly what the kernel would. Keeping the two apart is also what lets `read()` be tested here at all: `elfload` picks its ELF machine from the compiler's architecture and stops with `#error` on the arm64 machine the host tests run on, so `read()` parses with `eftl::elf::Traits64`, the programs' class, whatever it is compiled for.

A program with more than one PT_GNU_RELRO is refused as `NotSupported`: a linker gives a program one, more come from linker scripts that scatter the RELRO sections (the kernel's own `kernel.ld` does), and a `Plan` takes one.

**Change**
```cpp
for (auto const header: elf.segments()) {
    if (header.type() == eftl::elf::SegType::Load) {
        // at most Plan::MAX_SEGMENTS, else OutOfRange
        program.m_segments[program.m_segment_count++] = Segment{p_vaddr, p_offset, p_filesz, p_memsz, p_flags};
    } else if (header.type() == eftl::elf::SegType::GnuRelro) {
        // a second one is NotSupported
        program.m_relro = Relro{.vaddr = header.p_vaddr, .size = header.p_memsz};
    }
}
```

**Reference**
Zircon `elfldltl::LoadInfo` and its `RelroBounds`, filled from the program headers by `elfldltl::DecodePhdrs()`; Linux `load_elf_binary()` reads the same headers with `load_elf_phdrs()`.

**Verification**
- Host tests: `program` 7 of 7 (segments in file order with every field, RELRO size from `p_memsz` when `p_filesz` is shorter, no RELRO, the numbers building a `Plan` with the expected four mappings, two RELROs refused, nine segments refused, not an ELF and truncated program headers refused); `plan` still 36 of 36.
- `clang-tidy -p out/phys-host-tests` on both new files: no diagnostics in the library's files.
- `boot_check.py`: both images boot; nothing in them uses the library yet.

**Deliberately not done**
- The executor itself, the next step.
- Making `elfload` build on the host: its machine should be the target's, not the compiler's (`docs/repairs/261002/elfload.md` item 1).

**How it was found**
None; K23.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which program headers does the planner need, and which does it leave to the binder?
- Why does `read()` not decide whether the file can be started, and who does?
- Why is the RELRO's size `p_memsz` and not `p_filesz`?

## Title: The launcher starts a program from user space: launch::start() carries out the plan through system calls (2026-10-02)

**Files**
- `system/lib/launch/include/launch/launch.hpp`, `system/lib/launch/src/launch.cpp` — `launch::Request`, `launch::Started` and `launch::start()`, the executor.
- `system/lib/launch/include/launch/program.hpp`, `system/lib/launch/src/program.cpp`, `system/lib/launch/test/program_test.cpp` — `Program::stack_size()`, PT_GNU_STACK's p_memsz, and a host test for it.
- `system/lib/launch/BUILD.gn`, `system/lib/launch/test/BUILD.gn` — the planner is `:plan`, built for the host too; `:launch` is defined only in the user toolchains; the headers depend on elfload's.
- `examples/shell/shell.cpp`, `examples/shell/BUILD.gn` — `make_launcher()`, `launch_program()` and `print_launch_report()`; the control probe is started by the launcher, which gets a verdict line of its own; a probe's answer that cannot be read is no answer.
- `kernel/lib/syscall/test/fault_probe/fault_probe.cpp` — a table of more than two pages of relocated pointers in writable data, checked before any probe runs: what makes the boot take the launcher's snapshot path.
- `docs/repairs/261002/launch.md` — new: four items the review left open.
- `docs/design/260923/userspace-interface.md` — D13 revised.
- `docs/design/260924/userspace-filesystem.md` — §4.6's loading and placement, and the K23 row, marked superseded.

**Symptom**
None; K23's executor. Until now only the kernel could start a program, through process_spawn_boot.

**Cause**
What the launcher has to build, as objects:
- The file VMO: the program's file, a Slice of the executable BootFS. Its read-only and code pages are mapped straight from it.
- The snapshot VMOs and the zero-fill VMOs. A writable segment is cut where its file bytes stop filling whole pages, or at its end when it has no .bss. The pages before the cut are a SnapshotOnWrite child of the file, writable and sharing the file's pages until written. The pages from the cut on are a zero-fill VMO, with the partial page's file bytes copied in, and the .bss after them. Either part can be empty: the shell, init and, before this change, fault_probe have writable segments whose data starts in the page it ends in, so no snapshot at all.
- The process and its root region, from process_create.
- The image region: one sub-region as long as all the segments together, placed by the kernel, allowing read, write and execute.
- The vDSO VMO, mapped whole into the root region, read and execute, sealed.
- The stack region: a guard page at its bottom, and the stack VMO mapped above it.
- The first thread.

What happens, in order:
1. The file is judged (`elfload::check_program()`), read (`Program::read()`) and planned (`Plan::build()`), and the stack is sized. Nothing exists yet, so a refusal leaves nothing behind.
2. process_create makes the process and its root region.
3. vma_allocate makes the image region; then the snapshot and zero-fill VMOs, and each partial page is copied into its zero-fill VMO.
4. The vDSO is mapped, and the binder binds the program's imports to it. Each relocation goes through the Writer: an offset in the image, to the plan's mapping holding it, to a vmo_write into that mapping's VMO.
5. Each mapping of the plan is mapped into the image region at `vaddr - first`, with its final protection.
6. The stack region, its guard, and the stack VMO mapped above the guard.
7. thread_create, then process_start at `entry + bias`, with the stack pointer, the bootstrap handle in %rdi and the vDSO's base in %rsi.

Three of these choices carry the design:
- Relocations are written before anything of the program is mapped, so each mapping is made once with its final protection. RELRO is read-only from the moment it exists, and the launcher needs no vma_protect, which does not exist yet. Only the vDSO is mapped first, because binding needs its address.
- One region for the whole image keeps the segments at their link-time distances wherever the kernel puts it. So one bias, `image_base - first`, is right for every segment, and every relocation the binder computes with it lands where it should.
- Every handle is held by an `Owned` that closes it on any early return. Closing the last handle to a process that never started destroys it with its address space (K23a), so a failure at any step leaves nothing behind.

The review found that this check could not fail a boot. boot_check fails only on an error line, a FAILED line or a missing end. A program the launcher got wrong dies before main, which the kernel logs as a warning. And without a launcher the control probe still answered, through process_spawn_boot. So the shell now ends the control with `shell: launcher checks passed` or a FAILED line naming what went wrong.

**Change**
The sequence is the seven steps above; `launch.cpp` names each. Two pieces carry the rest. The Writer routes a relocation to the VMO that holds its page, and refuses a page that comes straight from the file:

```cpp
auto address = self.first + offset;
// the plan's mapping that holds |address|, or InvArgs
if (holder == nullptr || holder->source == Source::File) {
    return Status::InvArgs;
}
auto const into = address - holder->vaddr;
auto const piece = std::min<u64>(left, holder->size - into);
ou::vmo_write(vmo, source, holder->vmo_offset + into, usize(piece));
```

Every step that fails says which, through the caller's diagnostics, before its status goes back:

```cpp
auto failed(Request const &request, std::string_view const step, Status const status,
            ustl::Option<u64> const value = {}) -> Status {
    request.diagnostics({.what = step, .name = ours::to_string(status), .value = value});
    return status;
}
...
return ustl::err(failed(request, "vma_map of a segment", status, mapping.vaddr));
```

The stack is the size PT_GNU_STACK asks for, rounded up to a page, or 64 KiB when it asks for none, as the interface design (§3.4) sets.

**Reference**
- Zircon `ld::RemoteDynamicLinker` (`sdk/lib/ld/include/lib/ld/remote-dynamic-linker.h:85-135`): Allocate, Relocate into the segments' VMOs, Load with RELRO read-only, Commit. That is the shape of steps 3 to 5.
- Zircon `elfldltl::VmarLoader::AllocateVmar()` (`src/lib/elfldltl/vmar-loader.cc:68-85`): one VMAR for the image, segments mapped Specific inside it.
- Zircon process_builder (`src/lib/process_builder/src/process_builder.rs`): the stack from PT_GNU_STACK or a default (`:506-524`), and the start (`:867`). Two departures from it are written in `launch.hpp`: `start()` starts the process before the caller writes processargs, and it hands back only the process and its root region.
- The kernel's `userabi::launch()` (`kernel/lib/userabi/src/userboot.cpp:602`), which still starts userboot and process_spawn_boot's programs in the same order of steps. It maps the image as one writable and executable range at the fixed USER_ASPACE_BASE (`:501-504`) and binds it after mapping (`:689-704`), the order this executor reverses.

**Verification**
- Host tests: `program` 8 of 8 (one new: the stack size from PT_GNU_STACK, and 0 for none or a size of 0); `plan` 36 of 36.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed, the launcher's verdict among them), rel PASS (13 passed, 10 skipped). The dbg log: `shell: starting the control probe with the launcher`, `fault_probe: unknown probe none`, `shell: control probe none answered "unknown"`, `shell: launcher checks passed`, and a `shell: launch: bound: ...` line per import.
- Mutation: the Writer dropping every relocation. dbg FAILs with `shell: control probe none died without answering (expected "unknown")` and `shell: launcher checks FAILED (its control probe did not answer "unknown")`. Restored.
- The snapshot path. Until fault_probe's table, no program in the tree had a whole page of writable file bytes, so the boot never made a snapshot: `vmo_create_child(SnapshotOnWrite)`, the Writer's snapshot branch and the mappings of a snapshot did not run. fault_probe's second writable segment is now `vaddr 0xbbb8, filesz 0x2260, memsz 0x34d8` (`llvm-readelf -lW`), so its plan has a 2-page snapshot, `[0xb000, 0xd000)`, and a zero-fill part from 0xd000 with the partial page copied in. The table's 1100 R_X86_64_RELATIVE relocations land in both parts.
- Mutation: the Writer dropping every write into a snapshot page. dbg FAILs with `fault_probe: a pointer in its writable data was not relocated where it lies`, `shell: control probe none answered "relocation"` and the launcher's FAILED line. The probes the kernel starts still pass `11 killed, 0 survived`, so the kernel's loader relocates the same table right. Restored, and dbg passes again.
- Not exercised: a relocation split across the cut. Relocations are 8-byte aligned and the cut is page-aligned, so none of today's programs has one.
- An adversarial review in four lenses, each re-checked by a skeptic. Fixed from it:
  - the GN gate, which did not keep `:launch` out of the host toolchain;
  - the header's error contract;
  - the launcher's verdict line;
  - the shell's own failure reasons, and a probe failing to start before the control;
  - PT_GNU_STACK, which was ignored against §3.4;
  - the failed step reported through the diagnostics;
  - the bound addresses printed, which nothing logged for a program the launcher starts;
  - the Zircon citation;
  - and, each in its own commit, `__cxa_pure_virtual` for user programs and the two binder refusals that said nothing.
- A second review of the result, in two lenses, each re-checked by a skeptic, found the untested snapshot path above. It also found:
  - the header's diagnostics and status contract, made exact;
  - a probe answer longer than the shell's buffer, read past its end when printed;
  - claims in this record and the repairs and design notes that the code did not bear out, corrected before commit.

**Deliberately not done**
- The probes that fault still start through process_spawn_boot (U6), and init still spawns the shell (U7).
- Only the dbg image runs the launcher: the rel image's shell is started by init and holds no job.
- Starting before processargs, and handing back only the process and its root region: the departures from process_builder above. Whether to split `start()` into a build and a start, as process_builder and BuiltProcess do, is the user's to decide.
- The image region is made without CanMapSpecific, which Zircon's `AllocateVmar()` passes. Whether to adopt Zircon's rule is the user's to decide (the vma_allocate review).
- `docs/repairs/261002/launch.md`: the vDSO and the program's bytes, neither checked against the VMO that is mapped; the BootFS paying for every snapshot; a system call per relocation.
- No cap on the stack a program asks for, as in Zircon's process_builder: it is the program's own address space, and a size with no room fails at vma_allocate.
- No ASLR: the kernel places everything at the lowest room that fits.

**How it was found**
None; K23.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why are the relocations written into the VMOs before anything of the program is mapped, and what would the other order need?
- Why does one bias serve every segment, and what keeps it so?
- Why could the shell's check not fail a boot before the launcher had a verdict line of its own?
