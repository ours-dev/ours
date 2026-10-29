# riscv-boot changes

## Title: the riscv boot shim reads the device tree and reports the memory map physload will receive (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/bootshim/shim.cpp` — reads the tree with `fdt::Tree`, converts it with `bootshim::DevicetreeTranslator`, prints the map, the initrd and the command line, fills a `LegacyPlatformInfo`, and parks; refuses a tree it cannot use, with the reason
- `kernel/arch/riscv/phys/BUILD.gn` — the `bootshim` source_set depends on the bootshim and fdt libraries, `//system/lib/libc` and `//kernel/phys:headers`

**Symptom**
None; new feature. Before, the shim printed the hart ID and the tree's address and parked, so physload, once loaded, would have had no memory map.

**Cause**
What the shim adds to the map, and what it leaves out, follows from who reads what, and when:

- Its own image, `[__executable_start, __executable_end)`. physload reads the map through `LegacyPlatformInfo::memory_ranges` for as long as it runs, and both the array and the platform info live in the shim: on its stack, which is in its `.bss`. If the image range stopped where the loaded file stops (the end of `.data`), the stack would be free RAM in physload's BootMem, and the first allocation there could overwrite the array physload is still reading. `phys.ld` puts `__executable_end` after `.bss`, and `.bss.boot`, the boot stack in `start.S`, is inside `.bss`: `llvm-nm` shows `BOOT_STACK_END`, `__bss_end` and `__executable_end` all at `0x802072f0`.
- The tree, `[a1, a1 + totalsize)`. The kernel walks it itself later (`Handoff::device_tree`), so nothing may allocate over it before then. Its own `totalsize`, not the 2 MiB slot QEMU put it in: the rest of that slot is RAM like any other.
- Not the initrd. physload reserves it as the boot package, and a Reserved entry from the shim would make that reservation fail and physload panic; the conversion's contract (`docs/changes/261028/phys-bootshim.md`) is that the map never touches the initrd's pages.

A tree the shim cannot convert stops the boot here, with the reason, rather than further down: a map with a dropped reserved range would hand firmware memory out as free, and every other refusal is a tree whose memory the shim cannot describe truthfully. physload would only panic later with less to go on.

Two link failures shaped the build, and both say something about what a bare image has:
- `__cxa_atexit` undefined. The first version kept the translator in a `constinit` static. `ustl::Option` has a user-provided destructor, so the translator's destructor is not trivial, and the compiler registers a static's destructor with the C++ runtime's `__cxa_atexit` from a generated initialiser (`_GLOBAL__sub_I_shim.cpp`), which this image neither links nor runs. The translator is now stack-local, as on the x86 shim, which is also where the platform info lives.
- `memcmp`, `memchr` and `strlen` undefined. `std::string_view`'s `==` and `find()`, which the tree reader is built on, and `string_view` from a C string (`ours::to_string(Status)`) reach the C library. The shim now links `//system/lib/libc`, the one C library every ring builds.

**Change**
The shim, after reading the tree:

```cpp
auto const image = PhysAddr(IMAGE_START);
std::array<bootshim::PhysRange, 2> const own{{
    {.base = image, .size = PhysAddr(IMAGE_END) - image},
    {.base = device_tree, .size = tree.blob().size()},
}};

bootshim::DevicetreeTranslator translator;
auto const conversion = translator.convert(tree, own, PAGE_SIZE);
```

then refuses a failed conversion or one that dropped anything, and builds what `obi_main()` takes:

```cpp
ours::phys::LegacyPlatformInfo const info{
    .acpi_rsdp = ustl::NONE,
    .memory_ranges = translator.ranges().data(),
    .memory_range_count = conversion.unwrap().converted,
    .cmdline = translator.cmdline(),
    .bootloader_name = {},
    .device_tree = ustl::Option<PhysAddr>(device_tree),
};
```

The report prints from `info` itself, so what is printed is what physload will read. Ranges print half-open, `[base, end)`, in hex without leading zeros.

The tree's length comes from its own header (`totalsize`), passed to `fdt::Tree::from()`, which checks the magic before it believes it. `__executable_start` and `__executable_end` are named `IMAGE_START` and `IMAGE_END` through `LINK_NAME`, as `kernel/phys/memory.cpp` does for physload's, so the code follows the naming rules for globals while the linker symbols keep their names.

Rejected:
- Reserving the image and the tree rounded out to whole pages: the conversion does not need it (BootMem merges same-type reservations and only the initrd is claimed in pages), and Zircon reserves both exactly.
- Keeping the translator static with a no-destroy attribute: the stack already lives as long as physload runs, and the x86 shim does the same.
- A `bootloader_name`: the tree does not name its loader. SBI's base extension (`sbi_get_impl_id()`) would say OpenSBI, but `arch/riscv/sbi.hpp` has only the legacy putchar so far, and a name is diagnostic only.

**Reference**
- Zircon `InitMemory()` (`zircon/kernel/phys/boot-shim/devicetree.cc`): the phys image `[PHYS_LOAD_ADDRESS, _end)` and the devicetree blob (`fdt.size_bytes()`) are the shim's special ranges; the blob is read with a bound of `numeric_limits<uintptr_t>::max()`, trusting its header for its length, as here.
- The x86 counterpart, `load_physload()` in `kernel/arch/x86/phys/bootshim/shim.cpp`: the translator and the `LegacyPlatformInfo` on the stack, because physload runs as nested calls on it.

**Verification**
- Built: `prebuilt/ninja/ninja -C out/bringup.riscv kernel.phys_riscv.phys.dbg_0/obj/kernel/arch/riscv/phys/bootshim/bootshim.bin`, with the target's own warning set (`-Wall -Wextra -Wconversion -Wshadow -Wmissing-declarations ... -Werror`) and no flag added. The target does not take the phys-module config, so riscv clang's unused `-fno-plt` never reaches it.
- clang-tidy (`-p out/bringup.riscv`) on `shim.cpp`: no diagnostics. clang-format: clean.
- QEMU (`-machine virt -m 256M -smp 1 -bios default -kernel bootshim.bin`), without `-initrd`:

```text
[riscv-bootshim] hart 0x0 dtb 0x8fe00000
[riscv-bootshim] memory map, 5 ranges:
[riscv-bootshim]   [0x80000000, 0x90000000) ram
[riscv-bootshim]   [0x80000000, 0x80040000) reserved
[riscv-bootshim]   [0x80040000, 0x80060000) reserved
[riscv-bootshim]   [0x80200000, 0x802072f0) reserved
[riscv-bootshim]   [0x8fe00000, 0x8fe02447) reserved
[riscv-bootshim] no initrd
[riscv-bootshim] command line: none
[riscv-bootshim] platform info ready; physload is not loaded yet, parking
```

- With `-initrd` of a 4096-byte file:

```text
[riscv-bootshim] hart 0x0 dtb 0x8fe00000
[riscv-bootshim] memory map, 5 ranges:
[riscv-bootshim]   [0x80000000, 0x90000000) ram
[riscv-bootshim]   [0x80000000, 0x80040000) reserved
[riscv-bootshim]   [0x80040000, 0x80060000) reserved
[riscv-bootshim]   [0x80200000, 0x802072f0) reserved
[riscv-bootshim]   [0x8fe00000, 0x8fe02493) reserved
[riscv-bootshim] initrd [0x88200000, 0x88201000), 4096 bytes
[riscv-bootshim] command line: none
[riscv-bootshim] platform info ready; physload is not loaded yet, parking
```

- Read against the requirements: OpenSBI's `[0x80000000, 0x80060000)` is Reserved in both, so BootMem keeps it out of free RAM; the shim's image, `.bss` and stack included, and the tree (9287 bytes without the initrd properties, 9363 with) are Reserved; the initrd is reported and deliberately not in the map; the boot without one says `no initrd`.
- With `-initrd` and `-append "kernel.phys.next=physboot aslr.disabled"`: `command line: "kernel.phys.next=physboot aslr.disabled"`, and the tree grows to `[0x8fe00000, 0x8fe024d0)`.
- A tree the shim must refuse: QEMU's `dumpdtb` fixture with `/chosen/linux,initrd-end` overwritten by NOP tokens, passed with `-dtb`; OpenSBI accepts it and the shim stops:

```text
[riscv-bootshim] hart 0x0 dtb 0x8fe00000
[riscv-bootshim] the device tree's memory map cannot be used: a property is not in the shape its binding gives it
[riscv-bootshim] refusing the device tree (InvArgs); halting
```

- `ours-verify --out out/x86-dtb`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed). x86 does not build this shim; the run shows the shared libraries still build and pass.

**Deliberately not done**
- Loading physload out of the initrd and calling `obi_main(initrd base, initrd size, &info)`: the integration step. `info` is built and printed, not used.
- Refusing a malformed blob was not run under QEMU: OpenSBI parses the tree before the shim and stops on a broken header itself. That path shares `refuse()` with the conversion refusal, which was run.
- Checking that the initrd starts on a page: physload's `BootPackageInput::from()` refuses one that does not, with its own message. QEMU puts it at `0x88200000`.
- The UART named by `/chosen/stdout-path`: the console stays the legacy SBI putchar, as decided for this stage.

**How it was found**
None; new feature. The two link failures above were read off `ld.lld`'s `undefined symbol` lines, each with the header line that referenced it (`tree.hpp:562` for `memchr`, `shim.cpp:0` in `_GLOBAL__sub_I_shim.cpp` for `__cxa_atexit`).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must the shim's reserved range reach the end of `.bss` rather than the end of the file the firmware loaded, and what would go wrong first if it did not?
- Why does a `constinit` static still need `__cxa_atexit`, and what does that say about initialisation and destruction being two separate questions?
- Why does the shim stop the boot on a tree it cannot convert instead of passing on what it could read?

## Title: the shim's boot stack is 256 KiB, since physload and physboot run nested on it (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/start.S` — `BOOT_STACK` from 16 KiB to 256 KiB, with the reason beside it

**Symptom**
None yet; physload has not run on riscv. The stack was 16 KiB, and the shim hands the rest of the boot this one stack: `LegacyPlatformInfo` and the range array sit in the shim's frame, and physload, physboot and the kernel's first instructions run as nested calls below them.

**Cause**
The size was enough for the shim alone. x86 learned what the rest of the chain needs: its boot stack, `EARLY_STACK` in `kernel/arch/x86/phys/gdt.S`, overflowed at 16 KiB under the module loader's BootFS work and corrupted the objects below it, and is 256 KiB since.

**Change**
256 KiB, the same as x86, decided by the user on 2026-10-28. The stack is in `.bss`, so the shim's reservation of `[__executable_start, __executable_end)` grows with it and physload still cannot hand it out; nothing else changes. The image the firmware loads does not grow, since `.bss` is not in the file, but the header's `image_size` does, telling a loader to leave the room.

**Reference**
x86's `EARLY_STACK` (`kernel/arch/x86/phys/gdt.S`) and the comment on why it is 256 KiB.

**Verification**
- `llvm-nm -n bootshim`: `BOOT_STACK` at `0x802032f0`, `BOOT_STACK_END`, `__bss_end` and `__executable_end` all at `0x802432f0`.
- The riscv boot check: the shim's reservation is now `[0x80200000, 0x802432f0) reserved`, the boot reaches `shim` as before.
- `ours-verify`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 152 host tests pass.

**Deliberately not done**
- A guard page below the stack. The MMU is off in the shim, and physload maps all of RAM read-write when it turns it on, so there is no unmapped page to put below the stack; an overflow would still corrupt what lies below it without any fault, as it did on x86. A guard page needs page tables that leave a hole, which is the kernel's business.

**How it was found**
The reviewer of lane `dtb` compared the shim's 16 KiB with x86's 256 KiB.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a bigger `.bss` change the header's `image_size` but not the size of `bootshim.bin`?
- What would a stack overflow have corrupted first here, given what lies below `BOOT_STACK` in the image?
- Why can no guard page protect this stack anywhere in the phys world, even after physload turns the MMU on?

## Title: a trap vector for the phys world, which reports a fault and parks instead of looping silently (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/start.S` — the vector `.Ltrap_entry`, installed in `stvec` before anything else runs, and a 4 KiB stack of its own
- `kernel/arch/riscv/phys/bootshim/shim.cpp` — `bootshim_trap()`: names the cause, prints `scause`, `sepc`, `stval` and the faulting `sp`, parks; a second entry parks without printing
- `.agents/skills/ours-verify/scripts/riscv_boot.py` — the report's line is a failure line, so the riscv check stops at once instead of waiting out its timeout

**Symptom**
None yet, but certain at the first run of physload: the shim set `stvec` to 0, and nothing after it sets it either. Any fault in the shim, physload or physboot jumps to address 0, which on `virt` holds nothing to fetch, so it faults again there, forever, with no output. The first boot of code that has only ever been compiled is exactly when that happens.

**Cause**
The shim cleared the firmware's vector and had nothing to put in its place. x86's phys has no trap handler either; on riscv the cost of not having one is higher, because nothing upstream reports a fault taken in S-mode (OpenSBI only reports traps it cannot hand down).

**Change**
- The vector is the shim's and serves the whole phys world. `stvec` keeps it through physload and physboot, neither of which installs one. After physload turns the MMU on it is still reachable, because physload maps RAM, the shim's image included, at the same addresses.
- It saves nothing and never returns: nothing this early can be resumed. It reads `scause`, `sepc`, `stval`, keeps the faulting `sp` for the report, and switches to `TRAP_STACK`, because a stack pointer gone bad is one of the faults it exists for.
- The report names the exception from the privileged specification's table of S-mode causes, or prints an interrupt's number, then parks. A fault inside the reporter would enter it again from the top of its fixed stack and print over itself forever, so a second entry parks at once.
- The decision to add it was the user's, 2026-10-28.

**Reference**
The RISC-V privileged specification: `stvec` (direct mode, base 4-byte aligned), `scause` (bit 63 set for an interrupt; exception codes 0 to 15), `sepc`, `stval`. A kernel's own vector saves the whole register file and returns with `sret`; this one does neither, on purpose.

**Verification**
- A temporary probe in `bootshim_main`, a load from `0x80000000` (OpenSBI's PMP region), not committed:

```text
[riscv-bootshim] hart 0x0 dtb 0x8fe00000
[riscv-phys] trap: load access fault (scause 0x5) at sepc 0x80200680, stval 0x80000000, sp 0x80243180; halting
```

  This also settles what was only inferred on 2026-10-27: an S-mode load from OpenSBI's region is a load access fault (code 5) with the address in `stval`. With the probe, the riscv check reported `FAIL  1 failure line(s) · reached: shim` at once rather than after its 10 s timeout.
- Without the probe the shim boots as before and parks; no trap line.
- `llvm-nm`: `.Ltrap_entry` at `0x80200094` (4-byte aligned), `TRAP_STACK_END` at `0x802448e0`, inside the image's reservation.
- `format_cpp.py --check` clean; `clang-tidy -p out/bringup.riscv` reports nothing in `shim.cpp`.
- `ours-verify`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 152 host tests pass.

**Deliberately not done**
- Saving registers and a backtrace. A frame-pointer walk would need the faulting `s0`, which this vector does not keep; add it when a fault's location by `sepc` alone stops being enough.
- A vector in physload or physboot of their own. One shared reporter is enough while all three run identity-mapped; the kernel installs its own.

**How it was found**
The reviewer of lane `phys` named it the first thing to add if the integration boot hangs silently; the user chose to add it before that boot rather than after.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the vector keep working after physload writes `satp`, and what would have to change for it to stop working?
- Why does the reporter switch stacks before it calls into C++?
- Why would a fault inside `bootshim_trap()` loop forever without the `TRAPPED` check, even though the trap stack is reset on each entry?

## Title: phys and the kernel assume only the SBI specification of the riscv firmware, and two comments stop tying the shim to OpenSBI (2026-10-28)

**Files**
- `.agents/skills/ours-kernel-design/references/boot.md` — a Firmware section: the rule, the legacy console as its one optional call, where the load address really comes from, and how the rule is to be proven
- `kernel/arch/riscv/phys/BUILD.gn` — the load address is where QEMU's `virt` loads a flat image, not where the firmware puts the shim
- `kernel/arch/riscv/lib/arch/include/arch/riscv/sbi.hpp` — the rule in the namespace comment; the legacy console is optional in the specification, not kept by every implementation, and it does return a status
- `docs/repairs/261028/riscv-boot.md` — item 1: boot under a second SBI implementation
- `.agents/tasks/riscv-boot-chain/PLAN.md` — the decision in the table

**Symptom**
Latent, cannot be triggered today. Two comments stated a dependence on OpenSBI that the code does not have, and one promised something the specification does not.

**Cause**
SBI is an interface specification; OpenSBI is one program that implements it, RustSBI another, as UEFI is a specification and OVMF one firmware that implements it. Code that relies only on the specification boots under any of them; code that relies on what one of them happens to do boots only under that one, and nothing reports the difference until the other one is tried.

- `BUILD.gn` said the shim runs "where the firmware puts it" and that "OpenSBI occupies the first 2 MiB". The firmware puts it nowhere. QEMU's `-kernel` loader places a flat image at the first 2 MiB boundary above the end of the firmware image, and tells the firmware where it is; OpenSBI then jumps there. `0x80200000` therefore holds for any firmware smaller than 2 MiB on `virt`, and for no firmware on a board whose RAM starts somewhere else. A reader who believed the old comment would expect a firmware change to move the shim and a board change not to, and both are backwards.
- `sbi.hpp` said the legacy `console_putchar` is kept by "every implementation". The specification deprecates the legacy extensions and lets an implementation leave them out. It is the one call in use that a conforming firmware may lack, which is exactly what a reader checking the rule needs to know. It also said the call returns nothing; it returns a status in `a0`, as `console_putchar()` below it already said.

**Change**
The decision is the user's, 2026-10-28: "只依赖SBI，不依赖OPENSBI". It is recorded where design decisions live, so the next riscv change is checked against it.

`BUILD.gn`, before:

```text
# The boot shim is linked at a fixed address, the first 2 MiB-aligned address
# above the firmware's own memory on QEMU's `virt` machine (RAM starts at
# 0x80000000 and OpenSBI occupies the first 2 MiB).
```

after:

```text
# The boot shim is linked at a fixed address, where QEMU's `virt` machine
# loads a flat image: the first 2 MiB boundary above the end of the firmware,
# which sits at the start of RAM (0x80000000) and is smaller than 2 MiB. That
# is a fact of the platform, not of any one firmware: another SBI
# implementation of that size lands the shim at the same address, and a board
# whose RAM starts elsewhere needs another one.
```

`sbi.hpp`, before:

```cpp
/// Legacy `console_putchar`: one byte in a0, nothing returned. Deprecated
/// by the specification, but every implementation keeps it, and unlike
/// the debug console it needs no probe first.
```

after:

```cpp
/// Legacy `console_putchar`: one byte in a0, a status back in a0. The
/// specification deprecates it and lets an implementation leave it out,
/// so it is the one call here a conforming firmware may lack. It is used
/// because, unlike the debug console, it needs no probe first.
```

The shim already read the firmware's own memory from the tree's `/reserved-memory` rather than from a constant, so no code changed.

**Reference**
- The RISC-V SBI specification: the legacy extensions chapter (deprecated, optional), and the base extension's `sbi_probe_extension()` (extension 0x10, function 3), which is how a caller asks whether any other extension exists.
- Linux `sbi_init()` (`arch/riscv/kernel/sbi.c`) probes each extension it uses and falls back to the legacy calls only under `CONFIG_RISCV_SBI_V01`. Zircon's riscv64 `kernel/arch/riscv64/sbi.cc` likewise calls the firmware only through SBI.
- QEMU `riscv_calc_kernel_start_addr()` (`hw/riscv/boot.c`) rounds the firmware's end up to 2 MiB on a 64-bit machine; `riscv_load_kernel()` loads a flat image there.

**Verification**
Comments and documents only, so the build products do not change.
- `format_cpp.py --check` clean on `sbi.hpp`, `format_gn.py --check` clean on `BUILD.gn`; `clang-tidy -p out/bringup.riscv` on `shim.cpp`, which includes `sbi.hpp`, reports nothing.
- riscv check: the shim builds and boots as before, prints the same five-range map and parks; `reached: shim`, the expected failure until the shim loads physload (`fake.obi` still stops at `-fno-plt` in user-riscv).
- x86 `ours-verify`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 152 host tests pass.
- `lint_docs.py` clean on the four documents.

**Deliberately not done**
- Booting under a second implementation, the only real proof of the rule: repair item 1 in `docs/repairs/261028/riscv-boot.md`. It needs a new firmware fetched into `prebuilt/`, outside this change.
- Replacing the legacy console with the debug console extension. The user chose the legacy call on 2026-10-26, and both widely used implementations provide it; the rule now names it as the one optional call, and the repair item says what to do if a firmware lacks it.
- Renaming the `opensbi` stage of `riscv_boot.py`. Test tooling chose OpenSBI itself (`-bios default`), so it may know its banner.
- The comments that name OpenSBI as an example (`start.S`, `shim.cpp`, `bootshim/BUILD.gn`): they say which firmware QEMU loads, not what the code relies on.

**How it was found**
The user asked whether Linux uses OpenSBI. In answering I said the shim's address came from OpenSBI's default jump address on QEMU, then checked how the image is actually loaded (`riscv_boot.py` passes the flat `bootshim.bin` to `-kernel`) and corrected it: the address is QEMU's placement. Listing what in the tree assumed OpenSBI found the two comments; the user then set the rule.

**The user's prediction**
None.

**Three questions you should be able to answer**
- If QEMU booted RustSBI instead of OpenSBI, where would the shim land, and what decides it?
- Why does every boot under OpenSBI passing still not show that the kernel depends only on SBI?
- What must the shim do before it can use the debug console extension that it need not do for the legacy `console_putchar`?

## Title: the riscv boot shim loads physload out of the initrd and calls it, and the riscv chain boots to the fake kernel (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/bootshim/shim.cpp` — `load_physload()`: the initrd as an OBI boot package, item 0 as an ELF image, `find_scratch()` for room, `load_at()`, `relocate()`, then the call into physload's entry with the package and the platform info; the shim runs its static constructors and installs the phys console first
- `kernel/arch/riscv/phys/BUILD.gn` — the shim links `//kernel/phys:bootshim.core` (the ELF loader, the console list, panic), `//kernel/lib/ccrt` and its own copy of `sbi_console.cpp`
- `docs/repairs/261028/phys.md` — new: physboot's handoff log line says `cr3` on riscv
- `docs/repairs/261028/fake-kernel.md` — new: the fake kernel's `memory ranges` line runs into its value

**Symptom**
The riscv check stopped at `reached: shim · initrd fake.obi`: the shim printed the map and parked with `physload is not loaded yet`.

**Cause**
Nothing was wrong; the shim had stopped where its first lane's scope ended. What the load needs is set by who reads what, and when, the same reasoning as the map's own shape (the first entry of this file):

- The initrd is the OBI package, and physload is its item 0, an ET_DYN image linked at 0. It has to be copied somewhere and relocated before anything can run it, and nothing can allocate yet: BootMem is physload's.
- Where it goes is the one decision, and `DevicetreeTranslator::find_scratch()` (`docs/changes/261028/phys-bootshim.md`) makes it from the map the shim already built: RAM no range of the map claims and not the initrd's pages, because physload reserves its own image as `PhysImage` and the package as `BootPackage`, and BootMem refuses a reservation over memory a reservation of another type holds.
- The load address is the run address. phys runs with the MMU off until physload turns it on, and then it identity-maps RAM, so `relocate(base)` with the physical base is right for the whole of physload's life.
- `load_at()` makes the image visible to instruction fetch before it returns (`docs/changes/261028/riscv-phys.md`, the `load_at()` entry), so the call into physload needs no fence of its own; `relocate()` writes only its writable segment.
- The platform info and the translator that owns the map stay locals of `bootshim_main()`: physload, physboot and the kernel's first instructions run as nested calls on the shim's boot stack, and physload reads both through `LegacyPlatformInfo` as long as it runs.

The phys runtime pieces the loader needs (`elf_image.cpp`, and with it logz4) log through a console list that a static constructor fills, so the shim now runs `ccrt::init_global_static_objects()` and `init_early_console()` first, as the x86 shim's `phys_main()` does. The shim's own lines still go straight to the firmware through `print()`; anything the runtime logs, a panic included, now reaches the same console.

**Change**
The end of `bootshim_main()`:

```cpp
// before
report(info, translator.initrd());

print("[riscv-bootshim] platform info ready; physload is not loaded yet, parking\n");
park();

// after
report(info, translator.initrd());

load_physload(translator, info);
```

and the load, which follows the x86 shim's `load_physload()` step for step:

```cpp
auto const container =
    obi::ContainerView::from(reinterpret_cast<u8 const *>(package.base), package.size);
auto const boot = ours::phys::BootObi::from(container.unwrap());
auto const item = boot.unwrap().executable();
auto const bytes = item.value().payload();

ours::phys::ElfImage image;
image.init({bytes.data(), bytes.size()});
auto const size = ustl::round_up(image.size(), PAGE_SIZE);
auto const target = translator.find_scratch(size, PAGE_SIZE);
auto const base = PhysAddr(target.value());
auto const entry = image.load_at(base);
image.relocate(base);

using Entry = void (*)(PhysAddr, usize, ours::phys::LegacyPlatformInfo const *);
auto const physload = reinterpret_cast<Entry>(entry.unwrap());
physload(PhysAddr(package.base), static_cast<usize>(package.size), &info);
```

(each step checks its result and refuses with the reason, as the rest of the shim does; the excerpt leaves the checks out). Two differences from x86, both from the map: the scratch search steps over claims instead of starting above a floor, and the shim refuses a missing initrd, since on riscv the firmware's initrd is the only way the package arrives.

The build gives the shim what x86's has: `//kernel/phys:bootshim.core`, and `sbi_console.cpp` compiled a second time into the shim, as x86 compiles `serial_console.cpp` into both its shim and physload, because physload's copy is built `-fPIC` for its own image. The shim's `.bss` grows to 0x65870 bytes and its image ends at 0x80283650, inside the range it reserves for itself (`__executable_end`, after `.bss`, checked with `llvm-nm`); no orphan section lands past it (`llvm-readelf -S`).

The boot then ran through every stage the plan expected to fix one by one, with nothing to fix: the shim placed physload at 0x80060000, just above the firmware's `/reserved-memory` regions; physload registered the map without a refusal, built its Sv39 root, identity-mapped RAM and installed it; it loaded physboot at 0x8ff8a000; physboot loaded the fake kernel at 0x8ff82000, mapped it at 0xffffffd040000000 and handed off; the fake kernel checked the handoff, whose memory map snapshot has 15 ranges. The program header PT_RISCV_ATTRIBUTES in physload and physboot, BootMem's capacity for the device-tree map, physload's reservations, and the riscv arch layer's satp install and handoff jump all held on their first real run.

Ruled out:
- Printing through logz4 in the shim instead of its own `print()`: the shim's lines before `init_early_console()` would need the emergency path anyway, and changing them is not on the way to the next stage.
- Reserving the initrd in the map: physload reserves it itself, under a type of its own, and a Reserved entry over it would be refused (`docs/changes/261028/phys-bootshim.md`, the first entry).

**Reference**
Zircon's riscv shim `PhysMain()` (`kernel/arch/riscv64/phys/boot-shim/linux-riscv64-boot-shim.cc`) calls `BootShimHelper::Boot()` (`kernel/phys/boot-shim/include/phys/boot-shim/devicetree.h`), which views the devicetree's ramdisk as the boot image, loads it with `BootZbi::Load()` into memory the devicetree's `memalloc::Pool` gives it, and enters it with `BootZbi::Boot()`. This tree's image is an OBI package whose item 0 is an ELF, not a ZBI kernel, so the load is the x86 shim's `load_physload()` (`kernel/arch/x86/phys/bootshim/shim.cpp`): `ElfImage::init()`, `load_at()`, `relocate()`, and a call with `(package base, package size, LegacyPlatformInfo const *)`.

**Verification**
- riscv check (`boot_check.py --out out/bringup.riscv`): `PASS  handoff ok at 0.1s · reached: handoff ok · initrd fake.obi`, four runs out of four. The log, from the shim's last lines:

```text
[riscv-bootshim] initrd [0x88200000, 0x8823c778), 247672 bytes
[riscv-bootshim] command line: none
[riscv-bootshim] physload at [0x80060000, 0x800ac000), entry 0x8007aba0
[debug] Allocate page table (p:0x8FFFF000, s:0x1000)
[debug] Map identically (0x80000000, 0x10000000)
[debug] Allocate page table (p:0x8FFFE000, s:0x1000)
[debug] Loaded phys module 'physboot' at 0x8FF8A000, entry 0x8FF8F978
[debug] Kernel image at 0x8FF82000, 8 pages, entry offset 0x2000
[debug] Kernel mapping [0xFFFFFFD040002000, 0xFFFFFFD040003000) <- 0x8FF84000 r-x
[info] handoff: jumping to kernel at 0xFFFFFFD040002000, cr3=0x8FFFF000
[fake-kernel] entered
[fake-kernel] device tree  0x000000008fe00000
[fake-kernel] kernel at    0x000000008ff82000
[fake-kernel] handoff ok
```

- `scripts/format_cpp.py --check kernel/arch/riscv/phys/bootshim/shim.cpp`: clean. clang-tidy (`-p out/bringup.riscv`): no finding in `shim.cpp`; the 46 it prints are in headers this change does not touch. `scripts/format_gn.py --check kernel/arch/riscv/phys/BUILD.gn`: clean.
- x86 `ours-verify`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (152 passed).
- Not run: a second SBI implementation (`docs/repairs/261028/riscv-boot.md` item 1), real hardware.

**Deliberately not done**
- The two defects the boot log shows, neither on the path: physboot's handoff line names riscv's root `cr3` (`docs/repairs/261028/phys.md`), and the fake kernel's `memory ranges` label has no space before its value (`docs/repairs/261028/fake-kernel.md`).
- Anything the plan rules out for "basically usable": SMP, the SBI debug console, a relocatable shim.

**How it was found**
None, a new feature: the riscv check's `reached:` line said where the chain stopped, and the shim's own `parking` line said why. After the change the check's log was read stage by stage against the plan's list of likely failures, and none occurred.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can the shim relocate physload for the physical address it loaded it at, when physboot maps the kernel at a different virtual address than it loads it?
- What would physload do if the shim placed it over the shim's own image, and at which call would that show?
- Why may the translator and the platform info live on the shim's stack although physload reads them long after the call, and why are they not statics?
