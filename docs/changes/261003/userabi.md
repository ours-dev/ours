# userabi changes

## Title: The vDSO has no writable segment (system-abi-surface step 10, part 1) (2026-10-03)

**Files**
- `kernel/lib/userabi/vdso/BUILD.gn` — the vDSO is linked with `-z rodynamic`.

**Symptom**
The vDSO had three loadable segments: read-only (the ELF header, `.dynsym`, the hash tables, `.dynstr`), code (`.text`), and a writable one holding `.dynamic`. The kernel never maps any of it writable, so the third segment's flags described something that never happens. And it stood in the way of the next part, which maps the vDSO by segment and trusts the code segment alone.

**Cause**
The objects:
- `.dynamic`: the table a dynamic linker reads to find a shared object's symbols and relocations, and on most systems writes back into (`DT_DEBUG`, for debuggers). So linkers put it in a writable segment by default.
- The vDSO: a shared object with no dynamic linker of its own and no relocations, mapped from the one VMO every process shares.

The events:
1. lld puts `.dynamic` in a writable `PT_LOAD`, its default, behind `GNU_RELRO`.
2. Nothing ever writes it here: the loader reads the vDSO's symbols from its file, and the mapping is read and execute.
3. The image carried a writable segment that only made its layout harder to reason about: a third page, and a third kind of segment for every mapper to handle.

**Change**
`-z rodynamic` puts `.dynamic` in the read-only segment. The vDSO now has two loadable segments, read-only at `[0, 0x722)` and code at `0x1730`, and its image is two pages instead of three:

```text
LOAD  0x000000 0x0000000000000000 ... 0x000722 R   0x1000
LOAD  0x000730 0x0000000000001730 ... 0x000158 R E 0x1000
DYNAMIC 0x0004f8 0x00000000000004f8 ... R
```

`llvm-readelf -r` finds no relocations, so nothing needs `.dynamic` writable.

**Reference**
Fuchsia's toolchain passes `-z rodynamic` for all user code by default (elfldltl's `svr4-abi.h` says so), and erases it only for the kernel (`zircon/kernel/switch/BUILD.gn`). Zircon's vDSO, as user code, is linked with it.

**Verification**
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped); the logs read "vdso laid out once, 2 page(s)" and "vdso mapped [A00000, A02000)".
- The vDSO ABI check builds clean: the exports are unchanged.

**Deliberately not done**
- The same flag for every user program, as Fuchsia does: their layout is not what this step is about.
- Mapping the vDSO by segment and trusting only its code: part 2.

**How it was found**
Step 10 of `.agents/tasks/system-abi-surface/DESIGN.md`: mapping by segment needs to know what the segments are, and the writable one had no reason to exist.

**The user's prediction**
None. The user asked for whichever option is the better engineering practice, not the simpler one.

**Three questions you should be able to answer**
- Why do linkers put `.dynamic` in a writable segment by default, and why does the vDSO not need that?
- What did the third page of the vDSO's image hold, and why is it gone?
- What would the kernel have to do with a writable segment when it maps the vDSO by segment?

## Title: The vDSO is mapped by segment, and only its code is executable and trusted (system-abi-surface step 10, part 2) (2026-10-03)

**Files**
- `kernel/object/include/ours/object/vmo_dispatcher.hpp` — `mark_vdso()` takes the vDSO's code segment (`VdsoCode`), and `vdso_code()` gives it back.
- `kernel/object/vma_dispatcher.cpp`, `include/ours/object/vma_dispatcher.hpp` — an executable mapping of the vDSO's VMO must be exactly its code segment, sealed, read and execute, once per address space, and only it registers the trusted range; any other mapping of it is read-only.
- `system/lib/elfload/include/elfload/program.hpp`, `src/program.cpp` — new `segment_pages()`: each PT_LOAD's whole pages and protection, planned by eftl's load policy at page granularity.
- `kernel/lib/kelfload/include/kelfload/loader.hpp`, `src/loader.cpp`, `BUILD.gn` — new `Loader::map_segments()`: an image mapped segment by segment, each with its own protection.
- `kernel/lib/userabi/src/userboot.cpp` — `lay_out_vdso()` checks the vDSO has exactly one executable segment and no writable one, and marks its VMO with the code; `map_vdso()` maps it by segment.
- `system/lib/launch/src/launch.cpp` — the launcher maps the vDSO by segment, in a sub-region of its own.
- `examples/shell/shell.cpp` — the vDSO mapping checks rewritten for the new rule, and a new probe in the list.
- `kernel/lib/syscall/test/fault_probe/fault_probe.cpp` — new probe `pf_exec_vdso_data`.
- `system/public/ours/syscalls.gni`, `syscall.hpp`, `docs/design/260923/userspace-interface.md`, `docs/repairs/260919/elfload.md`, `.agents/skills/ours-kernel-design/references/layering.md`, `.agents/tasks/system-abi-surface/DESIGN.md` — the texts that described the vDSO as mapped whole.

**Symptom**
The vDSO was mapped as one range, every page of it read and execute, and the whole range was registered as the place a `syscall` is accepted from. Its first page is data: the ELF header, `.dynsym`, the hash tables and `.dynstr`. That data was executable inside the one range the kernel trusts. Latent: neither data page held the bytes `0F 05` today. But a symbol value or a hash that came to contain them would have been a `syscall` no stub put there, accepted by the kernel.

**Cause**
The objects:
- The vDSO's image: since part 1, a read-only page (the headers and tables) and a code page (`.text`, the 20 stubs).
- The registration: the mapping of the vDSO's VMO that the kernel records as the address space's vDSO range (`VmaDispatcher::map()` → `set_vdso_code_range()`). `trapped_from_vdso()` checks every trap against it.
- The two loaders that map the vDSO: the kernel's `userabi::launch()` (through `kelfload::Loader`) and the user-space launcher.

The events:
1. The kernel laid the vDSO out as one VMO, and the rule for mapping it was "whole, from 0, sealed, read and execute".
2. Both loaders mapped it that way: one mapping, all of it executable.
3. The registered range was the whole image, data included.

`kelfload::Loader` could map an image only as one range with one protection, which its own comment called a simplification. So there was no way to map the vDSO otherwise without first giving the loader that ability.

**Change**
- The code segment comes from the vDSO itself. When the kernel lays the vDSO out, it asks `elfload::segment_pages()` for its segments. It refuses to boot unless there is exactly one executable segment and no writable one, and marks the VMO with that segment.
- The rule, in `VmaDispatcher::map()`:

  ```cpp
  auto const registers_vdso = vmo.is_vdso() && !!(options & VmOptions::PermExecute);
  if (vmo.is_vdso()) {
      if (!!(options & VmOptions::PermWrite)) { return InvArgs; }
      if (registers_vdso && (vmo_offset != code.offset || rounded != code.size ||
                             !Sealed || perms != Read | Execute)) { return InvArgs; }
  }
  ```

- Both loaders map it by segment, with each segment's own protection. The kernel does it through `Loader::map_segments()`. The launcher allocates a sub-region the size of the image and maps each segment into it at its offset.
- `segment_pages()` is built on eftl's `KernelElfLoadPolicy` with a 4 KiB granule. This is the route `docs/repairs/260919/elfload.md` item 1 named for mapping by segment, and that policy also refuses two segments sharing a page. The same two functions are what mapping a program by segment will use.

**Reference**
- Zircon: `VmAddressRegion::CreateSubVmarInner()` checks an executable mapping of the vDSO's VMO with `VDso::valid_code_mapping()`, which compares it with `kVdsoCodeStart` and `kVdsoCodeSize`. It refuses a second one and records the first as the address space's `vdso_code_mapping_`. Non-executable mappings of the VMO are unrestricted.
- The one difference is where the code segment comes from. Zircon generates it at build time, because its vDSO is linked into the kernel image. Here the vDSO is a separate file of the kernel package, so the kernel reads it from that file. A constant could disagree with the file; this cannot.

**Verification**
- `ninja -k 0` on `out/vmo-snapshot`: no error.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped). The shell's groups: 19 passed in dbg; 13 passed and 5 skipped in rel. The kernel logs "vdso laid out once, 2 page(s), code at +0x1000 (1 page(s))". "launcher checks passed": the launcher's by-segment mapping starts a working process.
- The shell's vDSO checks, each with the answer expected:
  - a writable mapping: AccessDenied;
  - the code not sealed, the data executable, and the whole image executable: InvArgs;
  - the data read-only and the whole image read-only: Ok;
  - the code sealed: Ok, and a second time AccessDenied;
  - another process: Ok;
  - a slice mapped executable: Ok, and still no vDSO for `process_start` (BadState).
- The new probe `pf_exec_vdso_data` calls the vDSO's first page and is killed by a page fault. Its error code is `0x15`: present, user, instruction fetch. Its rip and cr2 are both `0xa00000`, the vDSO's base. All 12 probes were killed, none survived, and `trap_outside_vdso` is still refused.
- No host test of `segment_pages()`. elfload stops with `#error` on anything but x86-64 and the build host is arm64. Its rounding and page-overlap refusal are eftl's, which `load_policy_test` covers on the host.

**Deliberately not done**
- Mapping a program by segment in the kernel's loader (`userabi::launch()` still uses `Loader::map()`, one range read, write and execute), and narrowing PT_GNU_RELRO: `docs/repairs/260919/elfload.md` item 1, whose progress note says what is left.
- Zircon's finer check, against the exact return address of each stub (`VDso::ValidSyscallPC`), which a range cannot give: the recorded deferral (design 4.7 item 7). The kernel could now read those addresses from the vDSO the same way it reads the code segment.

**How it was found**
Step 10 of `.agents/tasks/system-abi-surface/DESIGN.md`, from the expert panel's finding that the vDSO was mapped executable as a whole.

**The user's prediction**
None. The user asked for the better engineering choice, not the simpler one: the code segment read from the vDSO itself, and `-z rodynamic` (part 1).

**Three questions you should be able to answer**
- What could a program have done with data that is executable inside the trusted range, and why was it not possible today?
- Why does the kernel read the code segment from the vDSO's file rather than from build-time constants, as Zircon does?
- What does error code `0x15` on the new probe's page fault say, bit by bit?

## Title: The kernel maps a program by segment, with RELRO read-only, and no longer read-write-execute (2026-10-03)

**Files**
- `system/lib/elfload/include/elfload/program.hpp`, `src/program.cpp` — `segment_pages()` becomes `mapping_pages()` (`MappingPages`, `MappingPagesList`). A writable segment that RELRO starts gives two mappings, its wholly-RELRO pages read-only and the rest read-write. The launcher's rules are applied: no segment both writable and executable, none without access bits, none sharing a page or out of order, and RELRO only at the start of a writable segment.
- `kernel/lib/kelfload/include/kelfload/loader.hpp`, `src/loader.cpp` — `map_segments()` maps every mapping readable plus its own protection, and logs each one. `Loader::map()`, both overloads, is deleted: it mapped an image as one range, and has no caller left.
- `kernel/lib/userabi/src/userboot.cpp` — `load_program()` lays the image out with `Loader::load()` and maps it with `map_segments()`. The vDSO check counts a RELRO mapping as writable.
- `kernel/lib/syscall/test/fault_probe/fault_probe.cpp`, `examples/shell/shell.cpp` — three new probes, `pf_write_text`, `pf_exec_data` and `pf_write_relro`, which the shell expects to be killed.
- `kernel/object/include/ours/object/vma_dispatcher.hpp`, `system/lib/launch/include/launch/launch.hpp` — the comments that said userboot maps its image writable and executable.
- `docs/repairs/260919/elfload.md` item 1 and `docs/repairs/260905/userabi.md` item 1, the same defect recorded twice — deleted, fixed.

**Symptom**
Every program the kernel starts was mapped as one range, readable, writable and executable: userboot, and whatever `process_spawn_boot` starts (init, the shell, fault_probe). Its code could be written, its data executed, and its RELRO, the GOT and everything else the loader writes once, stayed writable for the life of the process. A probe proved each: on HEAD with only the new probes added, `pf_write_text`, `pf_exec_data` and `pf_write_relro` all survived.

**Cause**
The objects:
- A program's segments. lld gives every program here four: read-only (headers, read-only data), code, RELRO (a whole segment of its own, padded to a page), and data with `.bss`.
- `kelfload::Loader`, which laid an image out in one VMO and mapped the whole span with one protection.
- `elfload::Binder`, which writes relocations through that VMO, never through the program's mappings.

The events:
1. The loader was written to place an image somewhere, with per-segment protection left for later, as its own comment said.
2. Binding only works if the pages it relocates are writable: true by accident, since everything was.
3. So the cheap mapping stayed, and with it a writable, executable image. The user-space launcher maps by segment, with RELRO read-only from the start, so the same program got a different shape depending on who started it.

Step 2 was never true of this binder: it writes through the VMO. So nothing ties the mapping to binding, and every mapping can be made with its final protection before the image is bound, as the launcher does.

**Change**

```text
userboot, before                          userboot, after
[200000, 207000) rwx  (one range)         [200000, 201000) r--
                                          [201000, 203000) r-x
                                          [203000, 205000) r--  RELRO
                                          [205000, 207000) rw-
```

`mapping_pages()` judges every segment before it writes any mapping, so a refused image leaves nothing behind. It takes RELRO from eftl's plan (`relro_intervals()`), and rounds RELRO's end down: a page RELRO only partly fills also holds data that is written, so it stays read-write. These are `launch::Plan::make()`'s rules, so a program now gets the same mappings from the kernel as from the launcher.

The image is still one VMO. The Binder addresses an image by offsets from one origin (`elfload::Image::writer`), and a read-only page takes relocations all the same because they go through the VMO.

**Reference**
- Linux maps each PT_LOAD with its own `p_flags` (`load_elf_binary()` and `elf_map()` in `fs/binfmt_elf.c`), and the dynamic linker makes RELRO read-only after relocating (`_dl_protect_relro()` in glibc's `elf/dl-reloc.c`).
- Zircon's elfldltl maps each segment with its own protection (`LoadInfo` and `VmarLoader`), and `ld::RemoteDynamicLinker` relocates into the segments' VMOs before mapping.

**Verification**
- The shared worktree did not build during this change: another session's unfinished rework of the vDSO range in `kernel/mm` (`vm_aspace.hpp`) breaks it, and it is not part of this commit. So the change was verified on an export of HEAD (434ce216) with exactly this commit's files laid over it, built in its own `out` directory:
  - `ninja -k 0`: no error, no warning;
  - `boot_check.py`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped); the shell's groups 19 passed in dbg, and 13 passed and 5 skipped in rel; launcher checks passed;
  - `shell: probes done (15 killed, 0 survived)`;
  - `pf_write_text`: page fault, error code `0x7` (present, user, write) at the probe's own code;
  - `pf_exec_data`: `0x15` (present, user, instruction fetch) with rip and cr2 both in the data page;
  - `pf_write_relro`: `0x7` in the RELRO page.
- The same three probes on HEAD alone, the old one-range mapping, all SURVIVED. They test what they claim.
- The loader's log shows the four mappings above for userboot and the same shape for the shell.

**Deliberately not done**
- Enforcing W^X in `VmaDispatcher::map()` for every caller: the next commit, which this one makes possible, since no kernel caller asks for a writable, executable mapping any more.
- Sealing a program's mappings: the launcher does not seal them either; only the vDSO's must stay as mapped.

**How it was found**
`docs/repairs/260919/elfload.md` item 1, and `docs/repairs/260905/userabi.md` item 1 before it. The user chose it as the next step after the vDSO work.

**The user's prediction**
None. The user accepted both recommendations: the splitting in elfload, and W^X enforced in the dispatcher next.

**Three questions you should be able to answer**
- Why could the RELRO pages be read-only from the start, though the loader writes them after mapping?
- Why does a page that RELRO only partly fills stay writable?
- What does each of the three new probes prove, and why had each to be shown surviving on the old mapping?

## Title: userboot starts the next program with the user-space launcher; the kernel loads userboot only (2026-10-03)

**Files**
- `kernel/lib/userabi/userboot/userboot.cpp` — userboot now:
  - takes its VmaRoot;
  - makes a `launch::BootfsLauncher` from the BootFS, vmex and the vDSO's VMO;
  - starts `userboot.next` with `launch_from_bootfs()` instead of `process_spawn_boot`, and closes the process handle and the executable copy once the message is written;
  - prints the launcher's errors (`print_launch_error()`).
- `kernel/lib/userabi/userboot/BUILD.gn` — depends on `//system/lib/launch`.

**Symptom**
None: step U8 of `docs/design/260924/userspace-filesystem.md`, under the user's rule that user space uses nothing on the kernel side but system calls. userboot was the last user program that asked the kernel to load the next one (`process_spawn_boot`). It starts init on the rel image and the shell on the dbg image.

**Cause**
The objects:
- userboot: the one program the kernel loads, with `userabi::launch()`.
- What the kernel hands it: the root job, the BootFS, vmex, the vDSO's VMO, the Debug resource and its own root region (VmaRoot).
- The BootFS launcher, which needs exactly the BootFS, vmex, the vDSO's VMO and a root region to map into.

The events:
1. The kernel loads userboot.
2. userboot makes the launcher, mapping the BootFS into its own address space and making an executable copy of it.
3. It launches `userboot.next` and writes the processargs, handing on the same handles it got.
4. It exits. Its address space goes with it, the BootFS mapping included. The executable copy is closed first.

Before this change, step 3 was the kernel's own ELF loader, run on userboot's request. That kept a second loading path in ring 0 alive for every boot. Its other users went in the previous commits: init's shell and autorun, and the shell's probes.

**Change**

```cpp
// before
ours::process_spawn_boot(job, name.data(), name.size(), there, &handles[0]);

// after
auto const started = launch::launch_from_bootfs(launcher, job, name, there, {...});
handles[0] = started.unwrap().root_vma;
auto _ = ours::handle_close(started.unwrap().process);
```

Without the BootFS, vmex, the vDSO's VMO or VmaRoot, userboot cannot start anything. It says so and exits 1. Before, a missing BootFS was allowed through to the next program.

**Reference**
- Zircon's userboot starts its next program itself: `launch_program()` in `zircon/kernel/lib/userabi/userboot/main.rs`, with the BootFS VMO it extracted.
- Its kernel loads userboot alone (`zircon/kernel/lib/userabi/userboot.cc`).

**Verification**
- `boot_check.py` on `out/bringup.x86`:
  - dbg PASS, 20 passed: `userboot: started shell`, 16 probes killed, launcher checks passed;
  - rel PASS, 18 passed: `userboot: started init`, `init: launched shell`.
- In both images, `object: process 'userboot' ... destroyed` follows the start.
- In both logs, the kernel's own loading lines (`userboot: found '<name>'`, `userabi: '<name>' starts with a bootstrap handle`) appear once each, for `userboot` alone.

**Deliberately not done**
- `process_spawn_boot` still exists, with one caller left: the shell's self-checks, when they have no launcher. Removing the call, the system call and the kernel's resident package is K25.
- The launcher's error printer now has three copies: the shell, init and userboot. Each is about ten lines and differs only in its prefix. The launch library prints nothing on purpose, so a shared printer would need a home that knows both `elfload` and the debug console.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which handles does the launcher need, and which of them did userboot not use before this change?
- Why can userboot exit right after writing the processargs without taking the next program down with it?
- After this change, which program does the kernel load, and through which function?
