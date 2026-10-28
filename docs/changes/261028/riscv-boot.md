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
