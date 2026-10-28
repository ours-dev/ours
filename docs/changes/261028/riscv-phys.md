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
