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

## Title: physload's riscv arch layer: Sv39 install, boot memory and the SBI console (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/aspace.cpp` — new: `Aspace::arch_install()` writes `satp` between two `sfence.vma` and checks the write took; `arch_setup_aspace()` identity-maps usable RAM below `ARCH_PHYSMAP_SIZE` and installs it
- `kernel/arch/riscv/phys/memory.cpp` — new: `BOOTMEM` and its two 100-entry arrays; `arch_init_memory()` sets up the aspace and reserves nothing of its own
- `kernel/arch/riscv/phys/sbi_console.cpp` — new: `SbiConsole`, registered as a static instance the way x86's serial console is; a strong `logz4::emergency_write`; `arch_uart_handoff()` returning `UartKind::None`
- `kernel/arch/riscv/phys/BUILD.gn` — `source_set("riscv")` compiles the three with `phys-module`'s `-fPIC` and re-exports riscv's arch, kernel and phys headers, as `source_set("x86")` does

**Symptom**
riscv physload failed to link: `ld.lld: error: undefined hidden symbol: ours::phys::arch_init_memory(ours::phys::Aspace*)`. The other arch symbols (`BOOTMEM`, `Aspace::arch_install()`) have default visibility for the module ABI, and `-shared` lets an undefined default-visibility symbol through without a word, so the one hidden symbol is the only error the linker gave.

**Cause**
`source_set("riscv")` was an empty placeholder: physload's generic code calls into an arch layer that did not exist.

**Change**
- `arch_install()`:

```cpp
auto const satp = ArchLowerPaging::satp(m_lower_pgd, 0);
asm volatile("sfence.vma\n\t"
             "csrw satp, %0\n\t"
             "sfence.vma" : : "r"(satp) : "memory");
```

  `satp` is MODE 8 (Sv39) in bits 63:60, ASID 0, and the root's page number. The fence before orders the stores that just built the tables ahead of the hardware walker's reads of them: the instruction after `csrw` is already fetched through the new tables, and riscv does not order ordinary stores with the walker's implicit reads without a fence. The fence after drops translations cached under the previous root; with translation off before, there are none, but `install(new_pgd)` exists to switch roots. Then `satp` is read back: `satp` is WARL, and a hart without Sv39 ignores the whole write, which would leave phys running untranslated until the jump to the kernel's high address faulted for no visible reason.
- `arch_setup_aspace()`: `init()`, `map_usable_ram(ARCH_PHYSMAP_SIZE)`, `install()`. Unlike x86 there are no allocation bounds for the root (x86 keeps it below 4 GiB for its application processors' 32-bit start) and no device memory to map (x86 maps the VGA hole): the console is an `ecall`.
- `arch_init_memory()` only calls `arch_setup_aspace()`. The ranges riscv needs kept off are reserved before it runs: physload's image and the package by `MemoryProvisioner`, and the firmware's memory, the device tree blob and the shim's image by the boot shim's reserved ranges (lane `dtb`, `kernel/arch/riscv/phys/bootshim/shim.cpp` on `feat/riscv-dtb`, which passes the shim image and the blob as Reserved). Reserving the blob here as well would collide: bootmem refuses a second reservation of a different type over the same bytes (`Status::AlreadyExists`).
- `SbiConsole` writes each byte with `arch::sbi::console_putchar()`; `read()` returns 0, like x86's VGA console, since nothing in phys reads input and the legacy getchar is not in `arch/riscv/sbi.hpp`. `emergency_write` uses the same loop, so a panic before the logger is installed still prints.
- `arch_uart_handoff()` returns `UartKind::None`. phys never found or configured a UART; it printed through the firmware, which the kernel can call the same way.

**Reference**
Zircon `kernel/arch/riscv64/phys/address-space.cc`: `ArchSetUpAddressSpace()` is `Init()`, `SetUpIdentityMappings()`, `Install()`, and `AddressSpace::ArchInstall()` writes `RiscvSatp` with the mode, root and ASID 0, then `arch::InvalidateLocalTlbs()`. Zircon's `ArchOnPhysLoadHandoff()` (`arch-on-physload-handoff.cc`) is empty, which is why `arch_on_phys_load_handoff()` keeps its weak no-op here. The fence before the `satp` write follows Linux `relocate_enable_mmu` (`arch/riscv/kernel/head.S`), which fences before loading the trampoline root for the same reason. x86's counterparts are `kernel/arch/x86/phys/{aspace,memory,serial_console}.cpp`.

**Verification**
- `ninja` builds `kernel.phys_riscv.phys.dbg_0/obj/kernel/phys/physload.bin` (153192 bytes). `llvm-nm -u physload` prints nothing; `llvm-readelf --dyn-syms` has no undefined entry beyond the null symbol; its 140 dynamic relocations are all `R_RISCV_RELATIVE`, the only kind the shim's relocation pass applies.
- `physload`'s segments are `R E`, `R`, `RW`, page-separated, plus `PT_NOTE`, `PT_DYNAMIC` and riscv's `PT_RISCV_ATTRIBUTES`, which the ELF load policy skips as it skips every non-`PT_LOAD` segment.
- physboot still fails to link, on `undefined hidden symbol: ours::phys::arch_handoff_to_kernel(...)`, which the next change defines.
- clang-tidy (`-p out/bringup.riscv`) on the three sources: nothing in riscv files. `scripts/format_cpp.py --check`: clean.
- Not run: nothing here has executed. physload first runs at the integration step, once the shim loads it.
- `ours-verify --out out/x86-phys`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed).

**Deliberately not done**
- A trap vector for phys. `stvec` is 0 from the shim, so a fault in phys jumps to address 0 and faults again, silently. Printing `scause`, `sepc` and `stval` would be the first thing to add if the integration boot stops without output.
- Keeping `no-map` reserved memory out of the identity map. OpenSBI's two `/reserved-memory` regions are `no-map` (its PMP denies S-mode access), but bootmem records them as plain reservations inside RAM, and `map_usable_ram()` maps all RAM. Nothing touches them, and QEMU does not speculate; on hardware a speculative access through a cacheable mapping is what `no-map` forbids.
- RAM above 64 GiB physical: not identity-mapped, and bootmem's top-down allocations would land there first. x86 has the same limit at 32 GiB.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which fence would you drop if you knew translation was off before the write, and what goes wrong if you drop the other one?
- Why did the linker report only `arch_init_memory` missing, when `BOOTMEM` and `Aspace::arch_install()` were missing too?
- What does `satp`'s WARL behaviour mean for a hart that implements only Bare mode, and how would that failure show up without the read-back?

## Title: physboot hands off to the kernel on riscv: fence the kernel's new mappings and its instructions in, then jump with the handoff in a0 (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/handoff.cpp` — new: `arch_handoff_to_kernel()` for riscv
- `kernel/arch/riscv/phys/include/ours/phys/arch_handoff.hpp` — the declaration's comment states the precondition on `pgd` and the register state the kernel starts with, gp and tp included
- `kernel/phys/BUILD.gn` — physboot compiles riscv's handoff source under `if (ours_cpu == "riscv")`, as it does x86's
- `docs/repairs/261028/riscv-phys.md` — new: item 1, the same missing `fence.i` in `ModuleLoader`, which loads physboot itself and is outside this lane's files

**Symptom**
The riscv physboot module failed to link: `ld.lld: error: undefined hidden symbol: ours::phys::arch_handoff_to_kernel(unsigned long, unsigned long, ours::phys::Handoff*)`.

**Cause**
`physboot_init()` ends by calling `arch_handoff_to_kernel(entry, global_aspace()->pgd(), handoff)`, and only x86 defined it. It has to be linked into physboot itself rather than bound from physload, since it is physboot's last act and x86's is wired the same way.

**Change**
What physboot hands over decided the shape. physboot does not build a new root: it maps the kernel's segments and the physmap into the aspace physload built and installed (`build_kernel_aspace()` calls `fix_mapping()` and `map()` on `global_aspace()`), and passes that aspace's root. So on riscv the "switch" is to the root already in `satp`, and what matters is the two fences, `sfence.vma` for the new mappings and `fence.i` for the new instructions, not the write:

```cpp
auto *const aspace = global_aspace();
ASSERT(pgd == aspace->pgd(), "riscv hands off on phys's own root 0x{:X}, not 0x{:X}",
       aspace->pgd(), pgd);
aspace->install();

register usize const argument asm("a0") = reinterpret_cast<usize>(handoff);
register VirtAddr const target asm("t0") = entry;
asm volatile("fence.i\n\t"
             "mv ra, zero\n\t"
             "mv s0, zero\n\t"
             "jr %[target]"
             : : [target] "r"(target), "r"(argument) : "memory");
```

- The kernel's image and physmap entries were written into live tables after physload installed them. The base privileged specification lets a hart keep a translation it looked up as invalid until an `sfence.vma` (the Svvptc extension exists to waive exactly that), so without a fence the jump to the kernel's high address may fault. `install()` is reused instead of a second copy of the `satp` sequence: it writes `satp`, fences on both sides, and checks the write took.
- `fence.i` before the jump. physboot wrote every byte of the kernel's text with ordinary stores (`ElfImage::load_at()` copies the segments and zeroes the bss, `relocate()` patches the image). RISC-V does not keep a hart's instruction fetches coherent with its own stores: an instruction cache may still hold whatever those bytes were, and only `fence.i` (the Zifencei extension, which phys's `-march=rv64ima_zicsr_zifencei` already includes) makes the stores visible to this hart's later fetches. x86 needs nothing here because its instruction fetches snoop stores; that is why x86's handoff has no counterpart and why this was easy to leave out. It sits inside the same asm as the jump, after the last store to the image, so nothing the compiler schedules can come between them. QEMU's TCG translates from memory and never shows a stale fetch, so no boot under QEMU would have caught its absence; on hardware with an instruction cache the kernel's first instructions could be stale bytes.
- It runs in place, without x86's trampoline page. x86 copies two instructions into a bootmem page that is identity-mapped under both roots; physboot's own text already is, since ModuleLoader loaded it into bootmem memory inside the RAM physload identity-mapped. That argument holds only while `pgd` is phys's own root, so the `ASSERT` states it: a different root would first have to be shown to map these instructions.
- a0 is the handoff, whose pointer equals its physical address because phys runs identity-mapped. ra and s0 are zeroed so the kernel's first frame terminates a frame-pointer walk instead of linking back into physboot, whose pages the kernel may reuse. The operands are pinned to a0 and t0 so neither can be allocated to ra or s0, which the asm overwrites before the jump.
- sp is left on the boot stack, as x86 leaves it: the kernel's entry sets its own. gp and tp keep phys's values, and the contract in `arch_handoff.hpp` now says they are undefined: the kernel's entry must set gp before any code linked with relaxation runs and tp before any CPU-local access, since the riscv kernel may use both.

The kernel therefore starts at `entry` with a0 the handoff's physical address, ra = s0 = 0, sp on the shim's boot stack, gp and tp undefined, satp holding the aspace root with the kernel mapped, its image visible to instruction fetch, and (from the shim) interrupts masked and `stvec` 0.

**Reference**
Zircon `HandoffPrep::ArchDoHandoff()` (`kernel/arch/riscv64/phys/arch-handoff-prep.cc`) clears ra and s0, sets sp, gp and tp from the kernel's ABI, puts the handoff in a0 and `jr`s to the entry; Zircon's physboot maps the kernel into the address space already installed (`gAddressSpace->Map()` in `kernel/phys/handoff-prep-vm.cc`), so it writes no `satp` there either. This tree's kernel sets up its own stack, so sp, gp and tp are not passed. Zircon's `ArchDoHandoff()` has no `fence.i` because its `ElfImage::Load()` (`kernel/phys/elf-image.cc`) ends with `arch::GlobalCacheConsistencyContext`, whose riscv destructor is `fence.i` (`kernel/lib/arch/riscv64/include/lib/arch/cache.h`); this tree's `ElfImage::load_at()` has no such step, so the handoff carries the fence. x86's version is `kernel/arch/x86/phys/handoff.cpp`.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.riscv kernel.phys_riscv.phys.dbg_0/obj/kernel/phys/physload.bin kernel.phys_riscv.phys.dbg_0/phony/kernel/phys/physboot`: succeeds.
- Sizes (`llvm-size`): physload text 145146, data 1640, bss 154624 bytes, `physload.bin` 153192 bytes; physboot module text 70498, data 416, bss 2136 bytes, `physboot.bootfs` 357712 bytes.
- physboot imports 12 symbols (`llvm-readelf --dyn-syms`, `UND`): `ElfImage::{init,load_at,relocate,fix_mapping}`, `Aspace::map`, `Aspace::arch_install`, `ASPACE`, `BOOTMEM`, `HANDOFF`, `ours::vpanic`, `logz4::get_global_logger`, `logz4::vlog_dispatch`. Every one is a GLOBAL DEFAULT symbol defined in physload, i.e. in its export table. Its relocations are 9 `R_RISCV_JUMP_SLOT`, 3 `R_RISCV_64` and 2 `R_RISCV_RELATIVE`, all of which `ElfImage::relocate_and_bind()` accepts on riscv. physload itself has no undefined symbol and only `R_RISCV_RELATIVE`.
- The compiled handoff (`llvm-objdump`): compare `a1` with `ASPACE->m_lower_pgd`, call `Aspace::arch_install` through the PLT, then `mv t0, s3`, `mv a0, s2`, `fence.i`, `li ra, 0`, `li s0, 0`, `jr t0`. It is the only `fence.i` in physboot.module; physload has none.
- clang-tidy (`-p out/bringup.riscv`) on `handoff.cpp` and `arch_handoff.hpp`: nothing in riscv files. `scripts/format_cpp.py --check`: clean.
- Not run: nothing here has executed; physboot first runs at the integration step.
- `ours-verify --out out/x86-phys`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed).

**Deliberately not done**
- A kernel stack. Passing one means deciding where it comes from and how big it is, which is the kernel entry's contract; x86 does not pass one either.
- A trampoline page for a root other than phys's own. Nothing builds one, and the `ASSERT` makes the day something does fail loudly.
- The same fence for physboot itself. `ModuleLoader` copies physboot into bootmem and then runs its constructors and its entry with no `fence.i` in between, and the weak `arch_on_phys_load_handoff()` runs too late to help (after the constructors). The fix belongs at the end of the generic `ElfImage::load_at()`, outside this lane's files: `docs/repairs/261028/riscv-phys.md` item 1.

**How it was found**
The first version of this change had no `fence.i`; an independent review found it, by comparing the handoff with what the reference relies on before its jump and counting `fence.i` in both images with `llvm-objdump` (zero in each).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can the riscv handoff run from physboot's own text, and which single fact about `pgd` does that depend on?
- If the `sfence.vma` were dropped, which access would fault first, and why can it work on one hart and fault on another?
- Why does a QEMU boot never show the missing `fence.i`, and why does x86's handoff need no instruction of the kind at all?

## Title: riscv's arch_setup_aspace() sets the page-table allocation bounds, the only write that initialises them (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/aspace.cpp` — `arch_setup_aspace()` calls `aspace.set_allocation_bounds(0, 0)` before `aspace.init()`
- `docs/repairs/261028/riscv-phys.md` — new item 2: `Aspace`'s members have no initialisers, so the next architecture can make the same mistake

**Symptom**
Latent, found in review before anything ran: the riscv physload's `Aspace::alloc_page_table()` reads `m_allocation_lower_bound` and `m_allocation_upper_bound` (`ld` from offsets 0x10 and 0x18, compared with `bltu`), and nothing on riscv had ever written them. Depending on what the stack held, the first page-table allocation would have been confined to a meaningless range (failing, so `init()` leaves a null root), or the debug build would have stopped on `DEBUG_ASSERT(m_allocation_lower_bound <= m_allocation_upper_bound)` in the first `Aspace::map()`. Only leftover values that happened to be equal would have worked.

**Cause**
`Aspace` (`kernel/phys/include/ours/phys/aspace.hpp`) has no constructor and its four `PhysAddr` members have no initialisers, and physload creates it as a plain local, `Aspace aspace;` in `obi_main()` (`kernel/phys/physload.cpp`). Default-initialising such an object leaves the members indeterminate, and reading one is undefined behaviour. The two roots are written by `init()`; the two bounds are written only by `set_allocation_bounds()`, which each architecture's `arch_setup_aspace()` is expected to call. x86 calls it twice (`(0, 4_gb)` for the root, which its application processors reach before long mode, then `(0, 0)`), so on x86 the call does double duty without saying so.

The earlier change in this file ("physload's riscv arch layer") said: "Unlike x86 there are no allocation bounds for the root". That was right about the policy and wrong about the code. riscv needs no bound, but "no bound" still has to be written, as equal bounds (`alloc_page_table()` reads `lower < upper` as bounded and anything else as the whole of bootmem). Leaving the call out because the policy is "none" confused a setting with its default, and `Aspace` has no default.

**Change**
```cpp
auto arch_setup_aspace(Aspace &aspace) -> void {
    aspace.set_allocation_bounds(0, 0);
    aspace.init();
    aspace.map_usable_ram(ARCH_PHYSMAP_SIZE);
    aspace.install();
}
```

The comment beside the call says both things it does: it states "no bounds", and it is the only write that initialises the two members. The generic fix, giving the members `{}` initialisers so no architecture can forget, is in `kernel/phys/include/ours/phys/aspace.hpp`, outside this lane's files, and is recorded as repair item 2 instead.

**Reference**
Zircon's phys `AddressSpace` (`kernel/phys/include/phys/address-space.h`) holds its page-table allocation bounds as `ktl::optional<uint64_t>` members, which a class type's default constructor leaves empty, and empty means unbounded; so its riscv `ArchSetUpAddressSpace()` (`kernel/arch/riscv64/phys/address-space.cc`) needs no call. x86's `arch_setup_aspace()` is `kernel/arch/x86/phys/aspace.cpp`.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.riscv kernel.phys_riscv.phys.dbg_0/obj/kernel/phys/physload.bin kernel.phys_riscv.phys.dbg_0/phony/kernel/phys/physboot`: succeeds.
- `llvm-objdump -d` of `arch_setup_aspace`: `sd zero, 0x10(a0)` and `sd zero, 0x18(a0)` before the call to `init()`, the two offsets `alloc_page_table()` reads.
- Sizes: physload text 145154 (was 145146), data 1640, bss 154624; `physload.bin` still 153192 bytes.
- clang-tidy (`-p out/bringup.riscv`) on `aspace.cpp`: nothing in riscv files. `scripts/format_cpp.py --check`: clean.
- Not run: nothing here has executed; physload first runs at the integration step.
- `ours-verify --out out/x86-phys`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed); x86 compiles none of these files.

**Deliberately not done**
- `{}` initialisers on `Aspace`'s members: a generic change outside this lane, repair item 2.

**How it was found**
Review: the reviewer noticed that x86's `set_allocation_bounds()` calls were the only writes to the bounds, then read the riscv disassembly of `alloc_page_table()` loading them.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is `Aspace aspace;` in `obi_main()` not zero-filled, when a global `Aspace` would be?
- Why does x86 call `set_allocation_bounds()` twice, and which of the two calls would still be needed if the members had initialisers?
- How would this bug have shown up differently in the dbg and the rel image?

## Title: phys's build comments credit ModuleLoader's eager binding, not -z now, with keeping riscv's PLT stubs servable (2026-10-28)

**Files**
- `kernel/phys/BUILD.gn` — the comment above `config("phys-module")` and the one inside it say what serves a riscv PLT stub, and what `-z now` adds
- `docs/repairs/261028/riscv-phys.md` — new item 3: physload links with `-shared`, so a missing default-visibility arch symbol links silently

**Symptom**
The comments this lane added in "phys code is not compiled with -fno-plt on riscv" said `-z now` "is what keeps the stub servable" and that ModuleLoader fills every slot "under -z now". Review found both overstated. The same section's Change paragraph repeats the claim, and its second question ("Why does `-z now` matter more on riscv than on x86 for this loader?") rests on it.

**Cause**
`ElfImage::relocate_and_bind()` (`kernel/phys/elf_image.cpp`) binds `.rela.dyn` and then `.rela.plt` unconditionally, before ModuleLoader runs a single instruction of the module; neither it nor eftl reads `DT_FLAGS`/`DF_BIND_NOW` (`git grep` finds `BindNow` only in eftl's enum of dynamic tags). So every `R_RISCV_JUMP_SLOT` slot holds the bound address whether or not the module was linked with `-z now`. A PLT is dangerous only when a slot can still point at a lazy resolver at the moment the stub runs, and the loader rules that out by itself.

What `-z now` does do: lld sets `DF_BIND_NOW` and `DF_1_NOW`, a promise to a dynamic linker that nothing here reads, and, because no slot is ever written after load, lld may put `.got.plt` inside `PT_GNU_RELRO`. In physboot.module the RELRO segment holds `.dynamic .got .got.plt`. Nothing in phys write-protects RELRO today, so that is layout, not protection. The earlier text mistook the linker flag that describes the loader's behaviour for the cause of that behaviour.

**Change**
Before, above the config:

```text
# riscv has no -fno-plt (see the condition below), so there a call to an
# imported function does go through a PLT stub, and -z now is what keeps
# the stub servable.
```

After:

```text
# riscv has no -fno-plt (see the condition below), so there a call to an
# imported function does go through a PLT stub, and what keeps the stub
# servable is ModuleLoader binding every .rela.plt slot before the module
# runs, not -z now.
```

Inside the config, "Under -z now ModuleLoader fills every slot" becomes "ModuleLoader fills every slot", followed by the reason (`relocate_and_bind()` binds `.rela.plt` eagerly whether or not the module asks) and what `-z now` adds (`DF_BIND_NOW`, `.got.plt` in RELRO). No flag changes; `-z now` stays, since it describes the loader truthfully and costs nothing.

The earlier section's second question is better read as: "What would have to change in the loader for `-z now` to matter on riscv, and why would it still not matter on x86?" (answer: a loader that bound `.rela.plt` lazily, or only when `DF_BIND_NOW` is set; x86 compiles `-fno-plt`, so its modules have no PLT slots to leave unbound).

**Reference**
None for the comment. The flag semantics are lld's (`-z now` sets `DF_BIND_NOW`/`DF_1_NOW`; with it, lld treats `.got.plt` as a RELRO section).

**Verification**
- `llvm-readelf -d` on riscv physboot.module: `FLAGS SYMBOLIC BIND_NOW`, `FLAGS_1 NOW`. `llvm-readelf -l`: segment `GNU_RELRO` maps `.dynamic .got .got.plt .relro_padding`.
- `git grep -n "BIND_NOW\|BindNow"` over `kernel/phys` and `system/lib/eftl/include`: the enum constant in `eftl/elf/details/dyn.hpp` and this change's own comment in `kernel/phys/BUILD.gn`; no loader code reads it.
- riscv `ninja` on `physload.bin` and `physboot` after GN regenerated: `ninja: no work to do` (comments only).
- `scripts/format_gn.py --check kernel/phys/BUILD.gn` reports "unsafe: the result differs from the source by more than layout", as it does on the base commit, before this lane touched the file.
- `ours-verify --out out/x86-phys`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed).

**Deliberately not done**
- Rewriting the earlier section. Change records are append-only; this section is the correction.
- `-z defs` for physload's link: repair item 3, a generic link-flag change outside this lane's riscv-only scope in this file.

**How it was found**
Review: the reviewer read `relocate_and_bind()` and saw that it binds `.rela.plt` with no check of `DF_BIND_NOW`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- If a riscv module were linked without `-z now`, what would its `.got.plt` slots hold on disk, and what would they hold when its first instruction runs?
- Why does putting `.got.plt` in RELRO protect nothing in phys today?
- Why does x86's module have `R_X86_64_GLOB_DAT`-style GOT entries for its imported functions but no `JUMP_SLOT` ones?

## Title: riscv's arch_paging.hpp includes the header that declares PhysAddr (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/include/ours/phys/arch_paging.hpp` — includes `<ours/types.hpp>`

**Symptom**
None observed: the header compiles on its own. Review noted that it names `PhysAddr` without including anything that declares it.

**Cause**
`PhysAddr` reached the header only through `<arch/paging/riscv_pagings.hpp>`, which includes `<arch/types.hpp>`, which includes `<ours/types.hpp>` (`clang++ -H` on a file that includes only this header shows that chain). A header that leans on another header's includes breaks the day that header stops needing them, in a file whose author never touched it. The `_gb` literal is different: it is declared in `ours/prelude.hpp`, which the build force-includes into every translation unit (`-include ../../kernel/include/ours/prelude.hpp`), so it needs no include. x86's `arch_paging.hpp` has the same gap; it is outside this lane and left alone.

**Change**
```cpp
#include <ours/types.hpp>

#include <arch/paging/riscv_pagings.hpp>
```

**Reference**
None, include hygiene.

**Verification**
- A translation unit that includes only `<ours/phys/arch_paging.hpp>`, compiled with `aspace.cpp`'s riscv command line and `-fsyntax-only`: exit 0, before and after.
- `gn check out/bringup.riscv`: `Header dependency check OK`.
- riscv `ninja` on `physload.bin` and `physboot`: succeeds. clang-tidy (`-p out/bringup.riscv`, header filter on riscv's phys and kernel directories) on `aspace.cpp`, `handoff.cpp`, `memory.cpp` and `sbi_console.cpp`: exit 0, no diagnostic in a riscv file. `scripts/format_cpp.py --check`: clean.
- `ours-verify --out out/x86-phys`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed); x86 compiles none of these files.

**Deliberately not done**
- The same include in x86's `arch_paging.hpp`: outside this lane's files.

**How it was found**
Review.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a header use `_gb` without including anything, but not `PhysAddr`?
- What does `gn check` verify about an include, and why does it not catch a missing one?
- Which change elsewhere in the tree would have turned this into a compile error, and in which file would the error have appeared?

## Title: ElfImage::load_at() ends by making the loaded image visible to instruction fetch, so every image phys loads is safe to jump into (2026-10-28)

**Files**
- `kernel/lib/arch/include/arch/cache.hpp` — declares `arch::sync_instruction_fetch_range(addr, size)`, the per-architecture step that makes a CPU's own stores visible to its later instruction fetches
- `kernel/arch/riscv/lib/arch/include/arch/riscv/cache.hpp` — new: riscv's definition, `fence.i`
- `kernel/arch/x86/lib/arch/include/arch/x86/cache.hpp` — x86's definition, a compiler barrier only
- `kernel/arch/riscv/BUILD.gn` — lists the new header
- `kernel/phys/elf_image.cpp` — `load_at()` calls the hook after its last store into the image
- `kernel/arch/riscv/phys/handoff.cpp` — drops its own `fence.i`, which the hook now covers
- `docs/repairs/261028/riscv-phys.md` — item 1 deleted, the other two renumbered

**Symptom**
Latent, cannot be triggered today. On riscv hardware with an instruction cache, physload would run physboot's constructors and entry, and the riscv shim (about to load physload with the same `load_at()`) would run physload, possibly executing stale bytes. QEMU's TCG translates from memory and never shows a stale fetch. Before this change `llvm-objdump -d` found no `fence.i` in riscv `physload` and exactly one in `physboot.module`, in the handoff.

**Cause**
A CPU executes code through its instruction-fetch path, and on riscv that path is not required to see the hart's own stores: the unprivileged specification's Zifencei chapter makes `fence.i` the only instruction that orders a hart's earlier stores before its later fetches. A loader writes an image's code with ordinary stores (`memset` of the whole image, `memcpy` of each segment), so between the copy and the jump something must run `fence.i`, or the hart may fetch whatever an instruction cache held for those addresses, possibly a previous image's code, possibly garbage.

Before this change the fence existed only where one lane had needed it: in riscv's `arch_handoff_to_kernel()`, before physboot's jump into the kernel. `ModuleLoader::start()` ran physboot's constructors and its `PHYS_MODULE` entry right after `load_at()` and `relocate_and_bind()` with no fence, and the riscv shim was about to become a third caller. The problem with a fence at each call site is the invariant's ownership, not the instruction: "this range is safe to execute" is a fact about the load, and a fact with one owner per caller is a fact some next caller forgets. x86 hides the mistake: its instruction fetches snoop stores, so code that leaves the fence out works there and only breaks on the first architecture that does not snoop.

The weak `arch_on_phys_load_handoff()` hook could not carry the fence either, because it runs after `run_initializers()`, which already executes module code.

The one argument this placement needs is that nothing stores into the image's code after `load_at()` returns. `relocate()` and `relocate_and_bind()` do store into the image afterwards, but only into writable segments: lld refuses to emit a dynamic relocation against a read-only section unless told `-z notext`, and no phys or kernel link passes it (`grep -rn notext` over the GN files finds nothing; riscv `physload` has no `TEXTREL`). Code patching would break the argument, which is why the handoff's comment now says that whatever comes to write the kernel's text later must sync again itself.

**Change**
The hook is a CPU primitive, so it lives in the arch library beside `clean_cache_range()`, declared generically and defined inline by each architecture:

```cpp
// kernel/lib/arch/include/arch/cache.hpp
auto sync_instruction_fetch_range(void const *addr, usize size) noexcept -> void;

// kernel/arch/riscv/lib/arch/include/arch/riscv/cache.hpp
inline auto sync_instruction_fetch_range(void const * /*addr*/, usize /*size*/) noexcept -> void {
    asm volatile("fence.i" ::: "memory");
}

// kernel/arch/x86/lib/arch/include/arch/x86/cache.hpp
inline auto sync_instruction_fetch_range(void const * /*addr*/, usize /*size*/) noexcept -> void {
    asm volatile("" ::: "memory");
}
```

`load_at()` calls it after its last store, before it records the load address:

```cpp
arch::sync_instruction_fetch_range(reinterpret_cast<void const *>(phys_addr), size());

m_pa_load = phys_addr;
return ustl::ok(m_pa_load + entry_offset.value());
```

The range is unused on both architectures today (`fence.i` covers all memory; x86 needs nothing), but it is part of the interface because an architecture whose instruction cache is not coherent by range, arm64 with its data-cache clean and instruction-cache invalidate by address, needs exactly that range.

riscv's handoff drops its `fence.i`:

```cpp
// before
asm volatile("fence.i\n\t"
             "mv ra, zero\n\t"
             ...
// after
asm volatile("mv ra, zero\n\t"
             ...
```

It goes rather than stays because a second fence with a comment saying it is needed would tell the next reader that `load_at()` does not guarantee what it now guarantees, and they would copy it into the next loader. The kernel's text is written only by `load_at()`; the relocations after it touch writable segments only, by the argument above.

On x86 the hook emits no instruction. Intel's SDM (volume 3, 8.1.3, "Handling Self- and Cross-Modifying Code") lists two ways for a CPU to execute code it stored itself: jump to the new code, or execute a serializing instruction first. Entering the image is a call, which is the first. The empty asm keeps the compiler from moving the stores past the call.

Ruled out:
- A `fence.i` in `ModuleLoader::start()` only, as the repair item said: the shim and physboot's kernel load would each need their own, and the next loader would have to remember.
- A weak or arch-layer function in `kernel/arch/*/phys`: the fact is about the CPU, not about phys, and an inline definition costs x86 nothing, where a call to an empty out-of-line function would still be a call.

**Reference**
Zircon `ElfImage::Load()` (`kernel/phys/elf-image.cc`) ends with `arch::GlobalCacheConsistencyContext cache; cache.SyncRange(physical_load_address(), load_info_.vaddr_size());`, with the comment that later relocation does not touch executable segments; its riscv destructor is `fence.i` (`kernel/lib/arch/riscv64/include/lib/arch/cache.h`), its x86 destructor `SerializeInstructions()` (`kernel/lib/arch/x86/include/lib/arch/cache.h`), Intel's second option where this tree relies on the first. Linux riscv `local_flush_icache_all()` (`arch/riscv/include/asm/cacheflush.h`) is the same `fence.i`, and `flush_icache_range()` is built on it.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.riscv kernel.phys_riscv.phys.dbg_0/obj/kernel/arch/riscv/phys/bootshim/bootshim.bin fake.obi`: builds.
- `llvm-objdump -d -C` of riscv `physload`: one `fence.i`, at 0xdf28 inside `ours::phys::ElfImage::load_at(unsigned long)`; `physboot.module`: none, the handoff's is gone.
- `scripts/format_cpp.py --check` on the five C++ files: clean. clang-tidy (`-p out/bringup.riscv` on `elf_image.cpp` and `handoff.cpp`, `-p out/bringup.x86` on `elf_image.cpp`): no finding in a changed file. `scripts/format_gn.py --check kernel/arch/riscv/BUILD.gn`: clean.
- x86 `ours-verify`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (152 passed).
- x86 fake image (`IMAGE=out/bringup.x86/fake.obi scripts/qemu_boot.py`): `[fake-kernel] handoff ok`.
- riscv check: `reached: shim · initrd fake.obi`, as before; nothing on riscv runs `load_at()` yet.
- Not run: real riscv hardware, the only place the fence's absence would show.

**Deliberately not done**
- arm64's definition: the arm arch library has no `cache.hpp` and no phys build to use one.
- Syncing other harts: phys runs on one hart, and the hook says it covers only the calling CPU. A kernel that loads code another hart runs needs an IPI or SBI's remote fence.

**How it was found**
None, a repair item (`docs/repairs/261028/riscv-phys.md`, item 1), filed by the review of the riscv handoff, which counted `fence.i` in both images with `llvm-objdump`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the end of `load_at()` safe for the fence even though `relocate()` stores into the image after it, and what change to the build or to physboot would make it unsafe?
- Why does a missing `fence.i` never show under QEMU, and why does x86 not need one when it enters code it has just copied?
- Why does the hook take a range that neither of today's definitions reads?
