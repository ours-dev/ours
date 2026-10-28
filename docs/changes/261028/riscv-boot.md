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
