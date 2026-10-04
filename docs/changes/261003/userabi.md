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

## Title: userboot unpacks the user BootFS from the boot image the kernel hands it (2026-10-03)

**Files**
- `kernel/lib/userabi/src/userboot.cpp` — `make_boot_image_handle()` copies the whole boot image (the OBI container) into a committed VMO. `write_bootstrap()` puts that VMO in userboot's processargs as `VmoBootdata`, with `Basic | Read | Map`, which makes room for seven handles.
- `kernel/lib/userabi/userboot/userboot.cpp` — `unpack_bootfs()`:
  - maps the boot image read-only and finds its StorageBootFs item with `obi::ContainerView`;
  - decompresses the item into a fresh VMO mapped read-write (`obi::ItemView::materialize()`);
  - hands back a duplicate without `Write`.

  `main()` builds the launcher from that VMO and hands it on as `VmoBootfs`. It closes the kernel-built BootFS, which the kernel still sends for one more step, and closes the boot image once read.
- `kernel/lib/userabi/userboot/BUILD.gn` — depends on `//kernel/lib/obi:headers`.

**Symptom**
None: step 2 of `.agents/tasks/userboot-bootfs/PLAN.md`. The kernel built the BootFS user space starts from: it decompressed its package and copied the whitelisted files into a VMO. In Zircon, userboot does that from the boot image itself.

**Cause**
The objects:
- The boot image: the OBI container phys hands over, in physical memory the kernel reaches through the physmap.
- Its StorageBootFs item: the user BootFS, LZ4-compressed, added in step 1.
- `VmoBootdata` (0x1a, Zircon's `PA_VMO_BOOTDATA`): the handle type that carries the image to userboot.
- The unpacked BootFS VMO, and the two handles userboot holds to it, one writable and one read-only.

The events:
1. The kernel copies the image into a VMO and sends it to userboot.
2. userboot reads the container header with `vmo_read`, maps the image read-only, and lets `ContainerView` check it.
3. It takes the first StorageBootFs item.
4. It creates a VMO of the item's decompressed size, maps it read-write, and materializes the item into it.
5. It duplicates the handle without `Write` and closes the writable one.
6. It launches `userboot.next` from the read-only BootFS and hands that on.

Why `Write` goes before anything else sees the VMO: every program's code is mapped from slices of it, through an executable duplicate. A holder with `Write` could change the code of every program started from it. The kernel-built BootFS never carried `Write` either (`USER_BOOTFS_RIGHTS`).

Why a copy of the image rather than a VMO over its own pages, as Zircon's `CreatePhysVmoHandle()` makes: `vmo_read` answers NotSupported on a physical VMO here. That is decision D2 of the design.

**Change**

```cpp
// kernel: one more handle in userboot's processargs
handles[count] = make_boot_image_handle();
infos[count++] = handle_info(HandleType::VmoBootdata, 0);

// userboot: the BootFS is its own, not the kernel's
auto const unpacked = unpack_bootfs(image, vma);
auto _ = ours::handle_close(image);
auto const bootfs = unpacked.unwrap();
```

Two smaller choices:
- The item search keeps its result in a `ustl::Option<obi::ItemView>` rather than a `bool` beside an item. `ItemView` has no default constructor, and the Option is the found flag.
- The design planned on `//kernel/lib/lz4`, but `obi::ItemView::materialize()` decodes LZ4 itself (`obi/compression.hpp`, header-only), so userboot needs only `obi:headers`.

**Reference**
- Zircon's kernel hands userboot the ZBI as a VMO: `.zbi = CreatePhysVmoHandle(...)` in `zircon/kernel/top/handoff.cc`, pushed in `zircon/kernel/lib/userabi/userboot.cc`.
- userboot unpacks STORAGE_BOOTFS with `get_bootfs_vmo()` (`src/bringup/lib/userboot/rust/src/bootfs.rs`): create, map read-write, decompress, unmap.

**Verification**
- `boot_check.py`:
  - dbg PASS, 20 passed: `userboot: unpacked the BootFS from the boot image, 0x2e000 bytes`, `userboot: started shell`, 16 probes killed, launcher and object_get_info checks passed, `shell: bootfs has 3 entries`;
  - rel PASS, 18 passed: `0x35000 bytes`, `userboot: started init`, `init: bootfs has 4 entries` (5 before: the kernel-built BootFS also held `userboot`), `init: launched shell`.
- On the rel image's serial port, the shell's `ls` lists `config/init/startup`, `init`, `shell` and `vdso`: the step 1 item exactly.
- The first boot of this step failed to slice a program out of the unpacked BootFS. The builder aligned file data to 8 bytes; that was fixed first, in its own commit (`docs/changes/261003/bootfs.md`).

**Deliberately not done**
- The kernel still builds and sends its own BootFS, and the user programs are still in the kernel package. Both go in step 3.
- The read-write mapping is not unmapped: there is no `vma_unmap`. It goes when userboot exits, right after the launch.

**How it was found**
None: a planned step.

**The user's prediction**
None given.

**Three questions you should be able to answer**
- Why must userboot drop `Write` before it builds its launcher, not only before it hands the BootFS on?
- What does the kernel still read from the boot image after this change, and what does it no longer?
- Why does `unpack_bootfs()` read the container header with `vmo_read` before mapping the image?

## Title: The kernel no longer builds the user BootFS; its package holds only what it boots from (2026-10-03)

**Files**
- `kernel/lib/userabi/src/user_bootfs.{hpp,cpp}` — deleted: `make_user_bootfs()` and the whitelist reader.
- `kernel/lib/userabi/BUILD.gn` — the two sources removed.
- `kernel/lib/userabi/src/userboot.cpp` — `resident_bootfs` is gone, and `write_bootstrap()` sends no `VmoBootfs`. The processargs carry six handles again: VmaRoot, the root job, vmex, the vDSO, the boot image and the Debug resource.
- `kernel/lib/userabi/userboot/userboot.cpp` — no kernel-built BootFS to close.
- `kernel/kernel_package.gni` — the `user_bootfs` and `init_startup` options and their generated files are gone. `config/init/startup` now comes from `user_bootfs(init_startup = true)`.
- `kernel/products/image/dev/BUILD.gn`, `kernel/products/image/dev.dbg_1/BUILD.gn` — the user programs leave `kernel_package("ours")` for `user_bootfs("bootfs")`, with their comments and the toolchain assert.
- `system/user_executable.gni`, `kernel/lib/userabi/vdso/BUILD.gn` — the `bootfs_user_visible` metadata is gone; nothing reads it.
- `system/service/init/init_startup.gni`, `system/service/init/include/init/startup.hpp`, `kernel/include/ours/boot/options.hpp` — comments that described the whitelist.
- `docs/repairs/261003/userabi.md` — the package-buffer item updated: five files now, and a better fix, phys's copy.

**Symptom**
None: step 3 of `.agents/tasks/userboot-bootfs/PLAN.md`. After step 2 the kernel still built a user BootFS that userboot closed unread. The user programs also sat in the kernel package twice over, once for that BootFS and once in the image's StorageBootFs item.

**Cause**
The objects:
- The kernel package (StorageKernel). It is what phys boots the kernel from, and what the kernel loads userboot and lays out the vDSO from.
- The user BootFS (StorageBootFs), which userboot unpacks since step 2.
- The whitelist, `config/user_bootfs`. It was generated from `bootfs_user_visible` metadata and read by `make_user_bootfs()`. Its job was to keep the kernel's own files out of what user space got from a package that held both.

With two items, what user space gets is decided by which item a file is packed into. A list applied at boot is no longer needed, and neither are the backstop that refused the kernel's own files by place and name, the copy into a fresh VMO, or the rights mask on it. In Zircon, too, the kernel neither builds nor reads STORAGE_BOOTFS.

**Change**

```text
before: [KSTR] kernel — 9 files: config/init/startup, config/user_bootfs, init, shell,
        userboot, vdso, vmours/code-patches, vmours/image.elf, physboot
after:  [KSTR] kernel — 5 files: userboot, vdso, vmours/code-patches, vmours/image.elf, physboot
        [BTSF] bootfs — 4 files: config/init/startup, init, shell, vdso
```

The kernel's whole part in user space's files is now one copy of the boot image into a VMO for userboot (step 2).

**Reference**
- Zircon: the kernel hands userboot the ZBI (`zircon/kernel/lib/userabi/userboot.cc`), and userboot unpacks STORAGE_BOOTFS itself (`get_bootfs_vmo()`, `src/bringup/lib/userboot/rust/src/bootfs.rs`).
- Nothing in Zircon's kernel builds a BootFS.

**Verification**
- `boot_check.py` on `out/bringup.x86`:
  - dbg PASS, 20 passed: 16 probes killed, launcher and object_get_info checks passed, `shell: bootfs has 3 entries`;
  - rel PASS, 18 passed: `init: bootfs has 4 entries`, `init: launched shell`.
- Both logs have `userboot: kernel package holds 5 files`, and `userabi: handing userboot its processargs: its root VMA, the root job and the boot image`.
- `obit tree` shows the layout above for the rel image. The dbg_1 image's package has the same five files, with its own `vmours/image.elf`.
- On the rel image's serial port, the shell's `ls` lists `config/init/startup`, `init`, `shell` and `vdso`.
- `out/autorun-launch` (`with = [fault_probe]`, `autorun = "fault_probe+none"`): dbg and rel PASS. init sees five entries and starts the autorun probe, which reaches main.
- `git grep` finds no `make_user_bootfs`, `resident_bootfs`, `bootfs_user_visible` or `config/user_bootfs` under `kernel/`, `system/`, `examples/` or `build/`.

**Deliberately not done**
- The kernel still decompresses its own package for userboot and the vDSO, into a buffer it never frees. That is `docs/repairs/261003/userabi.md`, item 1. It names a better fix now: phys has decompressed the same package already.
- `BootOptions::kernel_name()` in vmours has no reader left. It stays, with its comment updated, since the option is part of the boot-options contract phys shares.

**How it was found**
None: a planned step.

**The user's prediction**
None given.

**Three questions you should be able to answer**
- What did the whitelist protect against, and why is it not needed once the image has two BootFS items?
- Which files must be in both items, and why each?
- What does the kernel still do with the boot image after this change?

## Title: The kernel views the package physload decompressed instead of decompressing it again (2026-10-03)

**Files**
- `kernel/phys/include/ours/phys/kernel_package_handoff.hpp` — `BootPackageHandoff::kernel_storage`: the range of the kernel package as physload decompressed it. `valid()` and `fits_within()` cover it too.
- `kernel/phys/kernel_package_loader.cpp`, `kernel/phys/include/ours/phys/kernel_package_loader.hpp` — physboot fills it from the storage physload handed it.
- `kernel/phys/include/ours/phys/kernel_package.hpp` — `KernelPackage::view()`: a package over bytes someone has already materialized.
- `kernel/include/ours/boot/handoff.hpp`, `kernel/top/handoff.cpp` — `boot::kernel_package()` takes no buffer. It views the handed-over range through the physmap.
- `kernel/lib/userabi/src/userboot.cpp` — `materialize_kernel_package()` and its never-freed buffer are gone.
- `docs/repairs/261003/userabi.md` — deleted. Its one item is this.

**Symptom**
`userboot_init()` decompressed the kernel package into a buffer of its own, from `new (mm::GAF_KERNEL) u8[]`, and never freed it: about 0.8 MiB on the rel image. physload had already decompressed the same package at boot, into bootmem it never releases. The system held two copies.

**Cause**
The objects:
- physload's copy. `ModuleLoader::init()` decompresses the StorageKernel item into bootmem of type PhysImage, to find physboot in it, and hands the range to physboot (`PhysLoadHandoff::kernel_storage_base/size`). physboot loads the kernel from it.
- The handoff to the kernel (`phys::Handoff`). Its `BootPackageHandoff` carried only the outer OBI range. Its comment said the package's contents were not retained, as if the scratch were thrown away.
- The kernel's PMM. It takes back only Transient bootmem (`memmap_init.cpp`), so PhysImage memory stays reserved, and readable through the physmap, for the life of the kernel.

So the contents were retained after all, only nobody told the kernel. It rebuilt them from the compressed item, and needed a buffer for that. The buffer could not be freed while anything read it. Freeing it later would have waited on `vfree()` being safe across CPUs (`docs/repairs/260908/heap.md`, item 2).

**Change**

```cpp
// before: in userboot_init()
out = new (mm::GAF_KERNEL) u8[bytes];
auto result = boot::kernel_package({out, bytes});        // decompresses

// after: the handed-over range, viewed in place
auto const &storage = handoff().boot_package.kernel_storage;
auto const *const bytes = mm::PhysMap::phys_to_virt<u8 const>(storage.base);
return phys::KernelPackage::view(item.unwrap(), {bytes, storage.size});
```

`KernelPackage::view()` takes the first `memory_size()` bytes of the range as the BootFS, since whoever allocated the range may have rounded it up.

Zircon goes further. Its physboot hands the kernel userboot and the vDSO as ready ELF images (`PhysHandoff::userboot` and `::vdso`, each a `PhysElfImage`), so its kernel never sees the package at all. That would take moving the vDSO's layout and userboot's segment plan into phys. It was not done here.

**Reference**
- Zircon `PhysHandoff` (`zircon/kernel/phys/include/phys/handoff.h`): `PhysVmo zbi`, `PhysElfImage vdso`, `PhysElfImage userboot`, filled by `handoff-prep.cc`.

**Verification**
- `boot_check.py`:
  - dbg PASS, 20 passed;
  - rel PASS, 18 passed.
- Both logs have `userboot: kernel package holds 5 files`, `vdso laid out once` and the usual starts, so the view finds every file the decompressed buffer did.
- `fake.obi`, booted with `qemu_boot.py`, prints `[fake-kernel] handoff ok`: `boot_package.valid()` holds with the new range.
- The OBI host tests pass.
- Not measured: the memory saved. Nothing logs vmalloc allocations or free pages at boot. The allocation itself is gone from the code.

**Deliberately not done**
- Zircon's fuller shape, with userboot and the vDSO as ELF images in the handoff (above).
- Returning physload's PhysImage scratch to the allocator. The kernel now reads from it.

**How it was found**
While planning `.agents/tasks/userboot-bootfs`: `ModuleLoader::init()` allocates the same item's `memory_size()` as `materialize_kernel_package()` did, and `memmap_init.cpp` frees only Transient ranges.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why could the kernel not simply free its decompressed buffer once userboot had started?
- What guarantees that physload's copy is still there, unchanged, when the kernel reads it?
- What would the kernel stop doing if phys handed it userboot and the vDSO as images, as Zircon's does?

## Title: The kernel takes the boot image once it has read it, and a late reader panics (2026-10-03)

**Files**
- `kernel/include/ours/boot/handoff.hpp`, `kernel/top/handoff.cpp` — `take_boot_image()` returns the image's range and sets a latch (`BootImage::Taken`). After that, `boot_package()`, and through it `kernel_package()`, panic.
- `kernel/lib/userabi/src/userboot.cpp`:
  - `make_boot_image_handle()` copies from the taken range;
  - `write_bootstrap()` encodes the processargs first, with the boot image's handle_info in a slot reserved for it, and only then makes the handle that takes the image.

**Symptom**
None: step 3 of `.agents/tasks/boot-image-pages/PLAN.md`. The next step gives the image's frames to userboot's VMO, which frees them when userboot closes it. From then on the kernel must not read the image, and nothing enforced that.

**Cause**
The readers of the boot image in the kernel, in order:
1. the boot options, copied out at init (`boot_options.cpp`);
2. the kernel package's item header (`kernel_package()`), early in `userboot_init()`;
3. the user-space options for userboot's environment (`collect_user_options()`).

The user-space options were read in `write_bootstrap()` after the boot image's handle was made. They are also views into the image's command-line item, encoded into the processargs later still. `boot_package()` went on describing the range for good, so any later caller would read frames that, after the next step, may belong to someone else.

**Change**

```cpp
auto take_boot_image() -> phys::PackagePayloadHandoff {
    ASSERT(BOOT_IMAGE == BootImage::Held, "the boot image was taken twice");
    BOOT_IMAGE = BootImage::Taken;
    return handoff().boot_package.package;
}
```

In `write_bootstrap()`, the VmoBootdata handle_info takes the last slot before encoding. The handle fills that slot after encoding, so the environment's views are used while the image is still the kernel's. The latch is a two-state enum rather than a `bool`, written once, on the boot thread, before any user thread exists.

**Reference**
Zircon `EndHandoff()` (`zircon/kernel/top/handoff.cc`): after it, nothing reads `gPhysHandoff`'s ZBI, whose pages belong to userboot's VMO.

**Verification**
- `boot_check.py`: dbg PASS (20 passed), rel PASS (18 passed).
- The `userabi: passing ... to userboot` lines are identical before and after.
- In a copy of HEAD with this change and one extra `boot::boot_package()` after the image is taken, dbg stops with `the boot image was read after it was handed to userboot`. That line was not kept.

**Deliberately not done**
- The adoption of the frames (step 4). Until then, the kernel still copies, now from the taken range.

**How it was found**
None: a planned step.

**The user's prediction**
None given.

**Three questions you should be able to answer**
- Why must the processargs be encoded before the boot image's handle is made?
- Which kernel readers of the boot image are there, and why do all of them run before the take?
- Why does a late `boot_package()` panic rather than return an error?

## Title: userboot's boot image VMO owns the image's own frames, as Zircon's CreateFromWiredPages() does (2026-10-03)

**Files**
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`, `kernel/mm/vm_object_paged.cpp` — `VmObjectPaged::create_from_wired_pages(gaf, base, size, out)`.
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `VmCowPages::add_new_page_locked(index, page)`, the general "this frame is now this node's page".
- `kernel/object/include/ours/object/vmo_dispatcher.hpp`, `kernel/object/vmo_dispatcher.cpp` — `VmoDispatcher::create_from_wired_pages()`, with `GAF_USER`, replacing `create_committed()`, whose last caller this was.
- `kernel/lib/userabi/src/userboot.cpp` — `make_boot_image_handle()` zeroes the slack after the image's last byte and builds the VMO from the image's frames instead of copying.
- `kernel/top/handoff.cpp`, `kernel/mm/include/ours/mm/pm_frame.hpp`, `kernel/mm/early_mem.cpp` — comments that the review found false or stale: which thread takes the image, who calls `clear_reserved()`, what `userboot_init()` does with the image.
- `.agents/tasks/boot-image-pages/DESIGN.md` — D4 changed to Zircon's handling, and the latch's threads corrected; `.agents/tasks/userboot-bootfs/DESIGN.md` and `RESULT.md` — the claim that Zircon hands userboot a physical VMO, corrected.

**Symptom**
None: step 4 of `.agents/tasks/boot-image-pages/PLAN.md`. The boot image, about 1.2 MiB, was copied into a fresh VMO for userboot. The image's own frames stayed reserved for good.

**Cause**
The objects:
- The image's frames. They are reserved (`Flags::Reserved`, Zircon's WIRED state), counted on their node and zone, and owned by nobody.
- `unwire_frame()`. It turns such a frame into an allocated frame with no owner, as Zircon's `pmm_unwire_page()` moves a page from WIRED to ALLOC.
- `VmCowPages::add_new_page_locked()`. It gives an allocated frame to a node as an Owned page, which the node frees when it dies (`~VmCowPages`, `drain_owned`, `free_frames`).

The events, at boot:
1. The kernel takes the image (step 3).
2. It zeroes [size, align_up(size)) through the physmap. phys reserved that frame whole (step 1) but left the slack, since the RSDP pointer points into the unreserved Multiboot info block, and ACPI has read it by now.
3. For each frame, it asserts the frame is Reserved, unwires it and adds it, under the new node's lock.
4. userboot gets the VMO with `Basic | Read | Map` and reads it exactly as before. It closes the VMO when it exits, and the frames go to the buddy.

The order and the failure handling are Zircon's: unwire, then add, and a failure of either is fatal. The user chose that over returning an error, to stay consistent with Zircon. The path runs once, at boot, and a page map that cannot hold about 300 pages means the system cannot boot anyway.

**Change**

```cpp
// before: a copy
object::VmoDispatcher::create_committed(align_up(size, PAGE_SIZE), vmo, rights);
vmo.dispatcher()->write(bytes.data(), 0, bytes.size());

// after: the frames themselves
std::memset(base + image.size, 0, size - image.size);
object::VmoDispatcher::create_from_wired_pages(image.base, size, vmo, rights);  // GAF_USER inside

// in VmObjectPaged::create_from_wired_pages(), under the node's lock
ASSERT(!!(page->flags() & page::Flags::Reserved), ...);
unwire_frame(page);
ASSERT(Status::Ok == cow_pages->add_new_page_locked(index, page), ...);
```

No interface was added beyond Zircon's own: `unwire_frame` is `pmm_unwire_page`, `add_new_page_locked` is `AddNewPageLocked`, `create_from_wired_pages` is `CreateFromWiredPages`. The dispatcher factory takes the place of `create_committed`.

**Reference**
- Zircon `VmObjectPaged::CreateFromWiredPages()` (`zircon/kernel/vm/vm_object_paged.cc`): `pmm_unwire_page()`, then `AddNewPageLocked()`, then `ASSERT`.
- Zircon `CreatePhysVmo()` (`zircon/kernel/top/handoff.cc`), which uses it for the data ZBI handed to userboot.
- Zircon `pmm_unwire_page()` (`vm/include/vm/pmm.h`): "Unwires a page and sets it in the ALLOC state".

**Verification**
- `boot_check.py`:
  - dbg PASS, 20 passed: `userabi: 316 frames of the boot image handed to userboot, not copied`, the BootFS unpacked (0x2e000 bytes), 16 probes killed, launcher and object_get_info checks passed;
  - rel PASS, 18 passed: 297 frames handed over, `init: launched shell`.
- The frame count matches the stamping: `Reserved 316 frames of boot package` on dbg, 297 on rel.
- Free frames after userboot exits, from the dbg `aspace ... gone` lines, against step 3: about 300 to 316 frames more, line by line, which is the image. The first run of this step gave exactly 316 at the first `fault_probe` line. Later runs vary by a dozen or so frames, from the dbg kernel's background allocations.
- Both reserved-frame guards stayed armed, the boot-time one and the runtime one in `PmNode::free_frames()`. Neither fired when userboot's VMO died, so every adopted frame had been unwired.
- `kshell_run.py vmosnapshot.test pmnode.test`: ok.
- An adversarial review (three reviewers, each finding checked by an independent skeptic) confirmed 10 findings, 7 distinct, all fixed here:
  - the VMO was built with `GAF_KERNEL`, while the `create_committed()` it replaces used `GAF_USER`. A snapshot child of the image, which userboot's rights allow, would then allocate its copies with the kernel class, which spins in the allocator's slow path when memory runs out instead of answering OutOfMem. The factory now takes a `Gaf`, and the dispatcher passes `GAF_USER`;
  - six comments that stated something false.
  - The review also found that ACPI reads the RSDP in `init_platform_early()`, before the page allocator, not in `init_platform()`. That corrects this change's comment and `docs/repairs/261003/x86-phys.md`.

**Deliberately not done**
- A release-build check that each frame is reserved. Zircon also asserts, and the one caller passes exactly the range phys reserved.
- The MBI/RSDP hazard itself (`docs/repairs/261003/x86-phys.md`).

**How it was found**
None: a planned step. The `GAF` mistake was found by the review before commit.

**The user's prediction**
None given. On D4 the user asked to follow Zircon.

**Three questions you should be able to answer**
- What state is a boot image frame in before `unwire_frame()`, after it, and after `add_new_page_locked()`, in our terms and in Zircon's?
- Why must the boot image VMO allocate with the user class, though the kernel builds it?
- Why is the slack zeroed in the kernel rather than in phys, and why is it safe to zero it then?
