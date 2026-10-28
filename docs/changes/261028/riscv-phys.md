# riscv-phys changes

## Title: the riscv phys headers that Handoff includes, so code that reads a Handoff compiles for riscv (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/include/ours/phys/arch_bootmem.hpp` — `BootMem` is `bootmem::PhysicalMemory`, as on x86
- `kernel/arch/riscv/phys/include/ours/phys/arch_handoff.hpp` — an empty `ArchHandOff` and the declaration of `arch_handoff_to_kernel()`
- `kernel/arch/riscv/phys/BUILD.gn` — `:headers` publishes both and depends on bootmem's headers

**Symptom**
Every riscv translation unit that includes `ours/phys/handoff.hpp` stopped at `'ours/phys/arch_bootmem.hpp' file not found`: physload's sources, physboot, and the fake kernel. Four pieces of riscv work about to run in parallel all include it.

**Cause**
`handoff.hpp` includes two headers each architecture supplies, and riscv's `:headers` target was an empty placeholder.

**Change**
The two headers, with the same content as x86's apart from the comment on `arch_handoff_to_kernel()`, which states the riscv obligation: install `pgd` in `satp` from code that stays mapped across the switch, then jump with the handoff in `a0`. The definition is not written; nothing calls it on riscv yet. `arch_paging.hpp`, the third header x86 has, is left for the change that adds riscv's page-table format, since its content is that format.

Landed on its own, before the parallel work starts, so that each piece branches from a tree where the shared header compiles instead of each adding its own copy.

**Reference**
None. These are this tree's own seams; x86's versions are in `kernel/arch/x86/phys/include/ours/phys/`.

**Verification**
- Compiling `fake_kernel.cpp` for riscv now gets past the includes and stops at its x86 port I/O inline assembly, which is the fake kernel's own riscv work.
- Compiling physload's and physboot's sources for riscv now fails only on `ours/phys/arch_paging.hpp` (4 files), `ours/arch/aspace_layout.hpp` (2 files) and `arch::boot_marker` in ccrt (1 file).
- `ours-verify`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 148 host tests pass; x86 does not read these files.

**Deliberately not done**
- `arch_paging.hpp` and the address-space layout: they wait for the Sv39 page-table format.
- `arch_handoff_to_kernel()`'s definition.

**How it was found**
Compiling the riscv physload, physboot and fake kernel objects one by one with `-Wno-unused-command-line-argument` added, to see past `-fno-plt`, which riscv clang ignores and `-Werror` turns into a failure in every file.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does an architecture-neutral header like `handoff.hpp` include per-architecture headers at all, rather than the architecture including it?
- Why must the code that writes `satp` stay mapped under both the old and the new root?
- Why land two copied headers as their own commit instead of inside the first piece of work that needs them?

## Title: arch::boot_marker on riscv writes through the firmware's console (2026-10-28)

**Files**
- `kernel/arch/riscv/lib/arch/include/arch/riscv/boot_marker.hpp` — `boot_marker()` as one legacy SBI putchar
- `kernel/lib/arch/include/arch/boot_marker.hpp` — includes it under `ARCH_RISCV`
- `kernel/arch/riscv/BUILD.gn` — the header joins riscv's `:headers`

**Symptom**
`kernel/lib/ccrt/src/runtime.cpp` failed to compile for riscv with `use of undeclared identifier 'arch'`: `init_global_static_objects()` writes a marker per global constructor, and the generic header included nothing on riscv. Every riscv image with C++ globals links ccrt: physload, physboot, and the fake kernel.

**Cause**
`arch/boot_marker.hpp` dispatches by `#if` per architecture, and only x86 had a branch.

**Change**
riscv's marker is one legacy SBI `console_putchar`, the riscv equivalent of x86's single `outb` to COM1: it needs no setup, so it works before any console object exists, which is the marker's whole point. Like x86's, it writes the constructor's index as a raw byte, so it shows up on the console as a control character.

Landed before the parallel work because both the phys arch layer and the fake kernel would otherwise each have had to add it.

**Reference**
None; the x86 version is `kernel/arch/x86/lib/arch/include/arch/x86/boot_marker.hpp`. The SBI call is the legacy console putchar extension (EID 0x01) of the RISC-V SBI specification.

**Verification**
- `ccrt.runtime.cpp.o` compiles for riscv's phys toolchain (compiled with `-Wno-unused-command-line-argument` added, for the `-fno-plt` reason above).
- `ours-verify`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 150 host tests pass.

**Deliberately not done**
- A marker that skips non-printable bytes. x86 does not either, and the markers are for a person reading a raw serial capture.

**How it was found**
The same object-by-object compile of riscv physload as the change above.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can the marker not go through the phys `Console` objects?
- What does the legacy SBI putchar cost per byte, compared with x86's `outb`?
- Why is the dispatch an `#if` chain in the generic header and not a per-architecture include directory?

## Title: phys code is not compiled with -fno-plt on riscv, where clang has no such option (2026-10-28)

**Files**
- `kernel/phys/BUILD.gn` — `config("phys-module")` adds `-fno-plt` everywhere but riscv, and its comment says what a riscv module's imported calls look like instead

**Symptom**
Every physload and physboot source failed to compile for riscv with `clang++: error: argument unused during compilation: '-fno-plt' [-Werror,-Wunused-command-line-argument]` (10 objects of `physload.core` in one build).

**Cause**
`-fno-plt` is a request to the code generator: call an imported function by loading its address from the GOT and jumping through it, instead of calling a PLT stub that does the same load. clang implements it only for x86-64 and arm64, whose code generators know how to emit that sequence. For riscv the option reaches nothing, so the driver reports it as unused; the phys warnings config has `-Werror`, which makes one unused flag an error in every file that uses the config. A one-file probe shows the difference: `--target=riscv64-unknown-none-elf -fPIC -fno-plt` warns, the same line for x86_64 and aarch64 does not, and the riscv object still calls the import as `auipc t1, 0` + `jr t1` with an `R_RISCV_CALL_PLT` relocation.

So on riscv a module's call to physload always goes through a PLT stub. That is still safe here: the stub is `auipc`/`ld` of its `.got.plt` slot and a `jalr` to what it loaded, and the only thing that makes a PLT dangerous in a boot image is a slot that still points at a lazy resolver. Modules link with `-z now`, and `ElfImage::relocate_and_bind()` already binds `.rela.plt` as well as `.rela.dyn`, with `R_RISCV_JUMP_SLOT` (type 5) among its accepted types, so every slot holds the bound address before the module's first instruction runs.

**Change**
Before:

```text
cflags = [
  "-fPIC",
  "-fno-plt",
]
```

After:

```text
cflags = [ "-fPIC" ]
if (ours_cpu != "riscv") {
  cflags += [ "-fno-plt" ]
}
```

The comment above the config also says that on riscv `-z now` is what keeps the PLT stub servable. Rejected: `-Wno-unused-command-line-argument` for riscv. It would compile, but it would also hide every other flag that does nothing on riscv, and keep a flag in the command line that pretends to promise something.

**Reference**
None for the flag itself. Zircon's physload modules use no symbolic dynamic linking at all and are compiled `-fPIE` (`kernel/phys/physload_module.gni`, the comment in `set_defaults("physload_module")`), so no PLT arises there; this tree binds a module's imports by name against physload's export table (`ours-kernel-design`, boot.md), which is why the question exists here.

**Verification**
- x86's compile line for `physload.core.physload.cpp.o` still carries `-fPIC -fno-plt` after GN regenerated.
- riscv: `ninja -k 0` on `physload.bin` and `physboot` no longer reports `-fno-plt`; the remaining failures are the missing `ours/phys/arch_paging.hpp` (4 objects) and `ours/arch/aspace_layout.hpp` (1 object), which the next changes add.
- `ours-verify --out out/x86-phys`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed).

**Deliberately not done**
- The comments in `kernel/phys/elf_image.cpp` and `kernel/phys/physload_module.gni` that say a module "is compiled -fno-plt": they are true on x86 and outside this lane's files. The code beside them already accepts riscv's PLT relocation.

**How it was found**
`ninja -k 0` on the two riscv targets and counting the distinct `error:` lines, then the one-file probe above against three targets.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does a riscv PLT stub do, instruction by instruction, and which of those would go wrong if its `.got.plt` slot were never filled?
- Why does `-z now` matter more on riscv than on x86 for this loader?
- Why is silencing the unused-argument warning a worse fix than not passing the flag?

## Title: the riscv kernel address-space layout, Sv39's top 256 GiB for the kernel and its bottom 256 GiB for user space (2026-10-28)

**Files**
- `kernel/arch/riscv/kernel/include/ours/arch/aspace_layout.hpp` — new: every constant x86's layout file defines, placed for Sv39, with static_asserts that the windows fit and do not overlap
- `kernel/arch/riscv/kernel/BUILD.gn` — `:headers` publishes it
- `kernel/arch/riscv/phys/BUILD.gn` — `:riscv` re-exports `//kernel/arch/riscv/kernel:headers`, as x86's phys source set does, since `kernel/phys/aspace.cpp` includes the layout

**Symptom**
`kernel/phys/aspace.cpp` and `kernel/phys/physboot.cpp` failed to compile for riscv with `'ours/arch/aspace_layout.hpp' file not found`.

**Cause**
The layout is per architecture and riscv had none. Generic code reads it in three ways, which is why every name has to exist and why they stay macros: physboot places the kernel at `KERNEL_LOAD_BASE` and maps the physmap at `KERNEL_ASPACE_BASE` for `ARCH_PHYSMAP_SIZE`; the kernel's mm builds its address spaces, physmap, vmalloc window and CPU-local window from them; and `kernel/mm/cpu_local.cpp` tests `#ifndef ARCH_CPU_LOCAL_VIRT_BASE`, which a `constexpr` would not satisfy. Assembly may include the file too, so the checks sit under `#ifndef __ASSEMBLER__`.

What shapes the numbers is Sv39 itself. It translates 39 bits and requires bits 63..39 to copy bit 38, so the only usable addresses are the bottom 256 GiB and the top 256 GiB. x86-64's kernel half is 512 GiB, and its vmalloc window at +64 GiB sits above a 32 GiB physmap; riscv RAM starts at 2 GiB on QEMU `virt`, so the physmap has to reach further and everything above it shifts.

**Change**
The layout, with each window's offset from `KERNEL_ASPACE_BASE`:

```text
0xffff'ffc0'0000'0000  +0        physmap, 64 GiB, physical [0, 64 GiB)
0xffff'ffd0'0000'0000  +64 GiB   CPU-local window, 64 MiB in 2 MiB slots
0xffff'ffd0'4000'0000  +65 GiB   KERNEL_LOAD_BASE, image plus up to 1 GiB KASLR bias
0xffff'ffe0'0000'0000  +128 GiB  vmalloc, 4 GiB
user: [2 MiB, 1 << 38), restricted [2 MiB, 1 << 37)
```

- Kernel half and user half are exactly the reference's: `KERNEL_ASPACE_BASE 0xffffffc000000000UL`, `KERNEL_ASPACE_SIZE (1UL << 38)`, `USER_ASPACE_BASE 0x200000UL`, `USER_ASPACE_SIZE ((1UL << 38) - USER_ASPACE_BASE)`, restricted `((1UL << 37) - USER_ASPACE_BASE)` under this tree's name `USER_ASPACE_RESTRICTED_SIZE`.
- The physmap is 64 GiB rather than x86's 32 because RAM begins at 2 GiB: it covers up to 62 GiB of RAM. It is a multiple of 1 GiB, so physboot maps it with 64 gigapage leaves in the root table and no lower tables.
- No user page is held back at the top, unlike x86-64. There the last page exists so that SYSRET can never return to a non-canonical address, which faults in ring 0. A riscv `sret` drops to user mode before fetching, so the same return faults as the user program's own fault.
- The asserts: the kernel half is the top 2^38 bytes and starts at the lowest address with bit 38 set; the physmap is at the base, from physical 0, whole gigabytes, and reaches 2 GiB; the CPU-local window is above the physmap, slot-aligned, a whole number of slots, and below `KERNEL_LOAD_BASE`; the image's region (1 GiB bias plus 1 GiB of image) ends below vmalloc; vmalloc ends inside the half; the user half ends at 2^38. Sums are written as offsets from `KERNEL_ASPACE_BASE`, because the half's end as an address wraps to zero.

**Reference**
Zircon `kernel/arch/riscv64/include/arch/kernel_aspace.h` for `KERNEL_ASPACE_BASE`, `KERNEL_ASPACE_SIZE`, `USER_ASPACE_BASE`, `USER_ASPACE_SIZE` and `USER_RESTRICTED_ASPACE_SIZE`. Zircon has no fixed physmap, CPU-local or vmalloc constants any more: physboot sizes the physmap from RAM (`kernel/vm/include/vm/physmap.h`, `gPhysmapSize`) and the kernel image is placed by an allocator. The other windows follow this tree's x86 file, `kernel/arch/x86/kernel/include/ours/arch/aspace_layout.hpp`.

**Verification**
- `clang++ --target=riscv64-unknown-none-elf -std=c++23 -fsyntax-only -Wall -Wextra -Werror -x c++` on the header: no diagnostics, so every static_assert holds.
- An assembly probe `li a0, KERNEL_LOAD_BASE` including the header assembles to `li a0, -0xbf` + `slli a0, a0, 0x1e`, which is 0xffff'ffd0'4000'0000.
- riscv `ninja -k 0` on `physload.bin` and `physboot`: `aspace.cpp` gets past the layout include and the remaining errors are only `ours/phys/arch_paging.hpp` (5 objects).
- clang-tidy (`-p out/bringup.riscv`, inferred riscv phys command) on the header: no diagnostics. `scripts/format_cpp.py --check`: clean.
- `ours-verify --out out/x86-phys`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed); x86 reads none of these files.

**Deliberately not done**
- Sizing the physmap from the device tree's RAM at boot. The constant is what x86 and the generic mm read today; a dynamic size is a change to `PhysMap` for every architecture.
- Keeping MMIO out of the physmap. Below 2 GiB the window maps the `virt` machine's devices with ordinary leaves; without Svpbmt the platform's physical memory attributes already make them I/O, and x86's physmap maps its holes the same way.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does Sv39 leave a 2^64 - 2^39 hole in the middle of the address space, and what does a load from inside it do?
- Why can the riscv user half run to the very top of its 256 GiB when the x86-64 one stops a page short?
- Why must these constants stay macros, and what would break first if they became `constexpr`?

## Title: phys's address space on riscv is one Sv39 root (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/include/ours/phys/arch_paging.hpp` — new: `ArchLowerPaging` and `ArchUpperPaging` are both the Sv39 scheme, and the lower (identity) and upper (physmap) physical ranges
- `kernel/arch/riscv/phys/BUILD.gn` — `:headers` publishes it and depends on `//kernel/arch/riscv:headers`, where the Sv39 scheme lives

**Symptom**
Five physload sources (`aspace.cpp`, `elf_image.cpp`, `elf_image_mapping.cpp`, `module_loader.cpp`, `physload.cpp`) and physboot failed to compile for riscv with `'ours/phys/arch_paging.hpp' file not found`, through `ours/phys/aspace.hpp`.

**Cause**
`Aspace` is generic over the architecture's paging schemes and takes them from this header: `arch::Paging<ArchLowerPaging>` and `arch::Paging<ArchUpperPaging>`, and `DUAL_ASPACE` is whether the two differ. riscv had none, because the Sv39 scheme it names did not exist until lane `paging` landed it.

**Change**
- Both aliases are `arch::paging::PagingDispatcher<arch::paging::RiscvPagingLevel::Level2>::Type`, which is `RiscvPaging<Level2, RiscvSatpMode::Sv39>`. riscv has one translation root, `satp`, for the whole address space, so the two are the same type and `Aspace::DUAL_ASPACE` is false, as on x86: `Aspace::map()` takes its single-root branch and every address, kernel half included, is looked up through one table.
- The four mapping constants are given riscv meanings: the identity map is RAM, from 2 GiB (QEMU `virt`) up to the 64 GiB the physmap reaches, so `LOWER_MAPPING_BASE = 2_gb`, `LOWER_MAPPING_SIZE = 62_gb`; the physmap is physical [0, 64 GiB), so `UPPER_MAPPING_BASE = 0`, `UPPER_MAPPING_SIZE = 64_gb`. A static_assert holds the identity range under 256 GiB, since Sv39's lower half ends there and a physical address above it cannot be mapped at its own address.

**Reference**
Zircon `kernel/arch/riscv64/phys/include/phys/arch/address-space.h`: `ArchLowerPagingTraits = arch::RiscvSv39PagingTraits` and `ArchUpperPagingTraits = ArchLowerPagingTraits`, so `AddressSpace::kDualSpaces` (`kernel/phys/include/phys/address-space.h`, `!is_same_v<LowerPaging, UpperPaging>`) is false. x86's version of this header is `kernel/arch/x86/phys/include/ours/phys/arch_paging.hpp`.

**Verification**
- riscv `ninja -k 0` on `physload.bin` and `physboot`: every physload source compiles; the build now stops at the link, `ld.lld: error: undefined hidden symbol: ours::phys::arch_init_memory(ours::phys::Aspace*)`, which the arch layer (next change) defines.
- `physboot.physboot.cpp.o` and `physboot.kernel_package_loader.cpp.o` compile when their compile lines are run by hand (`ninja -t commands`); ninja itself orders them after physload's link, for physload's build ID.
- clang-tidy (`-p out/bringup.riscv`) on the header and on `kernel/phys/aspace.cpp`: nothing in riscv files (the 45 warnings it prints are in generic paging headers this change does not touch). `scripts/format_cpp.py --check`: clean.
- `ours-verify --out out/x86-phys`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed).

**Deliberately not done**
- Making the four mapping constants mean more. Nothing reads `LOWER_MAPPING_*` or `UPPER_MAPPING_*`, on x86 either (`git grep` finds only their definitions); they are defined because x86's header defines them, with values that say what riscv's identity map and physmap actually cover.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does `Aspace::DUAL_ASPACE` decide inside `Aspace::map()`, and why is it false on both x86 and riscv but would be true on arm64?
- Why can physical memory above 256 GiB not be identity-mapped under Sv39, even though Sv39 leaves can point at 56-bit physical addresses?
- What tells the linker that physload is incomplete here, and why does it complain about `arch_init_memory` but not about `BOOTMEM`?
