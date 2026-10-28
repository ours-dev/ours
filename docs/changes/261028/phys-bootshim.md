# phys-bootshim changes

## Title: bootshim turns a flattened device tree into the memory map physload reads (2026-10-28)

**Files**
- `kernel/phys/lib/bootshim/include/bootshim/devicetree.hpp` — new: `DevicetreeTranslator`, from an `fdt::Tree` and the caller's own ranges to `obi::MemoryRange`s, plus /chosen's initrd and command line
- `kernel/phys/lib/bootshim/include/bootshim/conversion.hpp` — new: `Conversion`, moved out of `legacy_memory.hpp` so both translators share it without the device-tree path including Multiboot2
- `kernel/phys/lib/bootshim/include/bootshim/legacy_memory.hpp` — includes `conversion.hpp`; `convert()` now says what a drop costs there (memory, never correctness)
- `kernel/phys/lib/bootshim/BUILD.gn` — the headers depend on `//kernel/phys/lib/fdt:headers`; `devicetree-smoke` joins `:tests`, which `//:default` already runs
- `kernel/phys/lib/bootshim/test/devicetree_smoke.cpp` — new host test
- `kernel/phys/lib/bootshim/test/data/opensbi_virt.dtb` — new: the tree OpenSBI hands the riscv shim on QEMU `virt`

**Symptom**
None; new feature. The riscv shim receives a device tree address in `a1` and had nothing to turn it into the `LegacyPlatformInfo::memory_ranges` physload's `arch_detect_memory()` reads.

**Cause**
What shapes the conversion is the reader on the far side. physload hands each range to `register_range()` (`kernel/phys/memory.cpp`), which sends a Ram range to `BootMem::add_usable()` and a Reserved one to `BootMem::reserve(range, FirmwareReserved)`, and panics on any refusal. BootMem keeps the two in separate lists: usable RAM, and a sorted list of typed reservations. Reading `kernel/phys/lib/bootmem/include/bootmem/physical_memory.hpp` settles four questions:

- Overlapping reservations of the same type are merged (`reserve()` grows the new range over every same-type entry it touches). The device tree's reservations all become FirmwareReserved, so overlaps among them are harmless and the shim need not merge them.
- A reservation outside usable RAM is simply recorded; `reserve()` never consults the usable list, and `snapshot()` only reports reservations inside RAM. So no clipping to RAM is needed.
- Order does not matter: `add_usable()` and `reserve()` do not look at each other's list.
- A reservation over a range that a reservation of another type holds is refused with `AlreadyExists`. This is the one that bites.

physload reserves two things of its own after the map: its image (`reserve_boot_image()`, PhysImage) and the OBI package (`reserve_package()`, BootPackage, rounded out to whole pages by `BootPackageInput::reserve()`). On riscv the OBI package is the initrd. If the shim listed the initrd as Reserved, it would be FirmwareReserved by the time `reserve_package()` ran, BootMem would refuse the BootPackage reservation, and physload would stop at `Failed to reserve OBI package`. Hence the contract, written in the header: the shim does not reserve the initrd, physload does, and nothing Reserved in the map may touch the initrd's pages. Two sources could still violate it:

- The memory reservation block. A bootloader that loads an initrd may list it there (U-Boot's `fdt_initrd()` does). Those entries are cut around the initrd's pages, not refused: the bootloader is describing the very thing physload will claim. The cut uses whole pages, because physload claims whole pages: an entry covering the initrd's last, partial page would still collide.
- A /reserved-memory region or one of the caller's ranges (the shim's own image, the tree). Those have an owner for good, the firmware or the shim, so an overlap with the initrd means two owners for one page, and the conversion refuses it (`AlreadyExists`) instead of letting physload panic later.

The second difference from Multiboot2 is what a full array costs. A Multiboot2 map partitions memory: every byte has one type and ranges do not overlap, so a range that does not fit is only memory left unused. A device tree is layered: RAM is described whole, and the reserved regions lie inside it. Drop a reserved range and the RAM under it is handed to the allocator as free; on QEMU `virt` that is OpenSBI's own memory, which its PMP denies to S-mode, so the first allocation there faults. A dropped range is therefore fatal for the caller, not a warning. The conversion still counts everything past the array (`Conversion::dropped`), so the shim can say how many there were.

Smaller rules, each from the same reader:
- A zero-length entry claims nothing and BootMem refuses it (`Range::from()` fails, and physload panics), so it is left out. An entry whose end does not fit in 64 bits is refused, as BootMem would.
- A memory node's `reg` is decoded with its parent's (the root's) `#address-cells`/`#size-cells`; a /reserved-memory child's with /reserved-memory's own. With the wrong widths, two one-cell (address, size) pairs read as one two-cell pair: a different, wrong range, not an error.
- RAM is the root's children whose `device_type` is "memory". The binding puts memory nodes there, and a memory node further down would need its bus's `ranges` applied to give a CPU address, which nothing here does.

**Change**
The interface, beside `LegacyMemoryTranslator` and shaped like it (convert, then read the result; header-only so the host can build it):

```cpp
class DevicetreeTranslator final {
  public:
    [[nodiscard]]
    auto convert(fdt::Tree const &tree, std::span<PhysRange const> const reserved,
                 u64 const page_size) -> ustl::Result<Conversion, Status>;
    auto ranges() const -> std::span<obi::MemoryRange const>;
    auto initrd() const -> ustl::Option<PhysRange>;
    auto cmdline() const -> std::string_view;
};
```

The map is Ram from the memory nodes, then Reserved from the reservation block, /reserved-memory and `reserved`, at most `MAX_DEVICETREE_MEMORY_RANGES` (64). /chosen is read first, because the initrd decides how the reservation block is cut:

```cpp
auto const whole = range.unwrap().value();
if (initrd.is_none() || !whole.overlaps(initrd.value())) {
    append(whole, obi::MemoryRangeType::Reserved);
    continue;
}
if (whole.begin < initrd.value().begin) {
    append({.begin = whole.begin, .end = initrd.value().begin}, obi::MemoryRangeType::Reserved);
}
if (initrd.value().end < whole.end) {
    append({.begin = initrd.value().end, .end = whole.end}, obi::MemoryRangeType::Reserved);
}
```

`linux,initrd-start` and `linux,initrd-end` are read as one cell or two each, as the binding allows; one without the other, an end below the start, or a width other than 4 or 8 bytes is refused (`InvArgs`), and an empty initrd is none. `bootargs`, when present, must be one string. A failed conversion resets the translator, so a caller that ignores the error finds nothing to use.

Rejected:
- Reserving the initrd in the shim: physload's own `reserve_package()` would be refused. The test shows it (below).
- Sorting or merging in the shim: BootMem already does both, and a second copy of the merge rule is a second owner of it.
- Truncating on overflow, as the Multiboot2 path does: safe there, unsafe here (above).
- A page-size constant in the library: the library is machine-independent; the shim passes physload's `PAGE_SIZE`.
- `tree.find("/chosen")`: both nodes are read as `tree.root().child(...)`. `find()` splits its path with `std::string_view::substr()`, which can throw `out_of_range`; under `-fno-exceptions` that becomes a call to libc++'s `__libcpp_verbose_abort`, which the riscv shim, linking no C++ runtime yet, does not define, so the link failed. The binding puts both nodes at the root, so nothing is lost.

`Conversion` moved to its own header so the device-tree path does not include `multiboot/view.hpp`; its comment no longer names `MAX_LEGACY_MEMORY_RANGES`, and each translator says what a drop costs.

The test builds trees by hand with a small `Blob` writer and feeds every map to a `bootmem::PhysicalMemory` the way physload does: `register_range()`'s switch, then the initrd reserved as BootPackage in whole pages. A refusal there is a physload panic.

**Reference**
- Zircon `boot_shim::DevicetreeMemoryMatcher::OnNode()`, `HandleMemoryNode()` and `HandleReservedMemoryNode()` (`zircon/kernel/phys/lib/boot-shim/devicetree-memory-matcher.cc`): memory nodes as RAM and /reserved-memory children as reserved. Zircon recognises a memory node by its name ("memory"); this follows Linux `early_init_dt_scan_memory()` (`drivers/of/fdt.c`) and the binding, which key on `device_type`.
- Zircon `boot_shim::ForEachDevicetreeMemoryReservation()` (`zircon/kernel/phys/lib/boot-shim/include/lib/boot-shim/devicetree.h`): reservation block entries minus "exclusions", because bootloaders reserve the blob and the ramdisk there. Zircon excludes the phys image, the blob and the ramdisk, which it gives types of their own; here the shim's image and the blob are listed as Reserved, the same type BootMem merges, so only the initrd needs excluding.
- Zircon `InitMemory()` (`zircon/kernel/phys/boot-shim/devicetree.cc`): the phys image `[PHYS_LOAD_ADDRESS, _end)` and the blob as special ranges, and an overflowing reservation list is fatal (`ZX_ASSERT_MSG`). Zircon's array holds 512 ranges; this one 64, like the Multiboot2 translator.
- Zircon `DevicetreeChosenNodeMatcherBase::OnNode()` (`devicetree-chosen-matcher.cc`): initrd bounds of 4 or 8 bytes, and `bootargs`. Zircon does not check that the end is above the start; this does.
- U-Boot `fdt_initrd()` (`common/fdt_support.c`): adds the reservation block entry for the initrd that the cut is for.

**Verification**
- `devicetree-smoke` passes: both QEMU trees (QEMU's `dumpdtb` fixture from `//kernel/phys/lib/fdt` and the OpenSBI runtime tree, whose two regions `[0x80000000, 0x80040000)` and `[0x80040000, 0x80060000)` come out Reserved and, through BootMem, leave `[0x80000000, 0x80060000)` FirmwareReserved and `[0x80060000, 0x80200000)` free), and hand-built trees for overlapping reservations, reservations flush with RAM's first and last byte, a reservation outside RAM, two memory nodes (one with two `reg` entries) beside four decoys, 32-bit root cells, /reserved-memory with cell widths of its own, a /reserved-memory child without `reg`, no /reserved-memory, no initrd, 32-bit and mixed-width initrd bounds, an empty initrd, `bootargs`, the reservation block cut around a 0x1800-byte initrd, a firmware region and a caller's range over the initrd, zero-length entries, exactly 64 ranges and 64 + 6, a second conversion, and fifteen malformed trees.
- The fixture `opensbi_virt.dtb` is a 16384-byte dump of guest memory from an earlier boot, cut to the blob's `totalsize` (9363 bytes). It was checked by dumping it again: booting the shim with `-initrd` (4096 bytes) and saving guest memory with the QEMU monitor's `pmemsave 0x8fe00000 16384` gives the same bytes except `/chosen/rng-seed`, which changes every boot.
- Each check shown to bite: one guard at a time removed from `devicetree.hpp`, the test rebuilt and run (exit -6 is `require()`'s abort, -5 the trap of `ustl`'s `unwrap()` on an error or `value()` on none), then restored and passing again:

| Guard removed | Test result |
|---|---|
| reservation block cut around the initrd | fails in `Physload::take()`: BootMem refuses the BootPackage reservation |
| the initrd rounded out to whole pages | fails in `Physload::take()`, the same refusal |
| refusing an owned range over the initrd | a refused-tree check finds the tree accepted |
| leaving out zero-length entries | wrong count in `empty_entries_are_left_out` |
| refusing a range past 2^64 | a refused-tree check finds the tree accepted |
| counting ranges past the capacity | `dropped == 6` fails |
| /reserved-memory read with its own cell widths | `convert_ok()` gets an error |
| memory nodes read with the root's cell widths | `convert_ok()` gets an error |
| only device_type "memory" is RAM | wrong count in `two_memory_nodes` |
| a node without device_type is not RAM | wrong count on the QEMU tree |
| only the root's children are memory nodes | wrong count in `two_memory_nodes` |
| one-cell initrd bounds | `convert_ok()` gets an error |
| refusing a tree with no RAM | a refused-tree check finds the tree accepted |
| reset on failure | `ranges().empty()` fails after a refusal |
| one initrd bound without the other | exit -5 |
| an initrd end below its start | wrong status (OutOfRange, not InvArgs) |
| an empty initrd is none | `initrd().is_none()` fails |
| `bootargs` must be one string | exit -5 |
| page size must be a power of two | a refused-tree check finds the tree accepted |
| `reg` must be whole entries | a refused-tree check finds the tree accepted |
| reading /reserved-memory at all | wrong count on the OpenSBI tree |
| reading the reservation block at all | wrong count in `overlapping_reservations` |
| adding the caller's ranges | wrong count on the QEMU tree |

- clang-tidy (`-p out/x86-dtb`) on `devicetree_smoke.cpp`, and so on `devicetree.hpp`, `conversion.hpp` and `legacy_memory.hpp`: no diagnostics on these files (only the existing ones in bootmem's and multiboot's headers).
- The header also compiles for riscv's phys toolchain, in the riscv shim, with its `-Werror -Wconversion -Wshadow` set (the next change).
- `ours-verify --out out/x86-dtb`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (151 passed, `devicetree-smoke` among them).

**Deliberately not done**
- `status = "disabled"` /reserved-memory children are taken like any other; Zircon does not skip them either.
- /reserved-memory children that ask for an allocation (`size`, `alloc-ranges`, no `reg`) add nothing: no memory has been chosen for them yet.
- `ramoops` and other typed regions: everything reserved is FirmwareReserved.
- Translation through a bus's `ranges`, and memory nodes below the root.
- A blob writer shared with `//kernel/phys/lib/fdt`'s `tree_smoke.cpp`: this test has its own small one, because those test files belong to the fdt change; sharing them is a later cleanup.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why may a Multiboot2 memory map be truncated when it does not fit, while a device tree's may not?
- Why does the shim leave the initrd out of the map, and what exactly would physload do if it listed it as Reserved?
- Why is a /reserved-memory child's `reg` decoded with /reserved-memory's `#address-cells` rather than the root's, and what goes wrong, silently, if it is not?
