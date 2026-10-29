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

## Title: DevicetreeTranslator::find_scratch() finds room for the next image in the device tree's map (2026-10-28)

**Files**
- `kernel/phys/lib/bootshim/include/bootshim/devicetree.hpp` — `find_scratch(size, alignment)`, with `first_claim_over()` and `align_up()` behind it; the translator keeps the initrd's pages (`m_initrd_pages`) that `convert()` already computed
- `kernel/phys/lib/bootshim/test/devicetree_smoke.cpp` — the OpenSBI tree's answer, and hand-built cases for every claim the walk steps over, a second bank, the top of the address space, refused inputs and dropped maps; a `Physload::reserve_image()` that reserves the answer as physload reserves its own image

**Symptom**
None, a new function. The riscv shim is about to load physload out of the initrd and needs somewhere to put it; the x86 shim's `LegacyMemoryTranslator::find_scratch()` reads a Multiboot2 map, which a device tree platform does not have.

**Cause**
Where an image may go is decided by what physload does with the map afterwards, and that is stricter than "not inside a Reserved range of RAM":

- physload registers every Reserved range as `FirmwareReserved`, the boot package's pages as `BootPackage`, and the memory it runs from as `PhysImage` (`MemoryProvisioner::reserve_boot_image()`). `bootmem::PhysicalMemory::reserve()` merges only reservations of the same type and refuses, with `AlreadyExists`, one that overlaps a reservation of another type; physload panics on that. So the image must overlap no range of the map that is not Ram, not even by a byte, and none of the initrd's pages, rounded out the way `BootPackageInput::reserve()` rounds them.
- The image is copied out of the initrd by `ElfImage::load_at()`. A destination overlapping the source would have the copy read bytes it has already overwritten.
- The device-tree map is not normalised: Ram ranges come first, Reserved ones after, unsorted and possibly overlapping (the design comment on `DevicetreeTranslator` says why). A Multiboot2 map's Available entries are already free memory, so the x86 version can take the first Available entry above a floor; here a Ram range is free only where no claim covers it, so the search has to step around claims.

The x86 version protects what it must through a single `floor` (the highest end among the shim, the Multiboot2 block and the module), giving up all memory below it. On riscv that floor would be the device tree at the top of RAM, leaving nothing above it: the claims have to be avoided one by one.

**Change**
First fit, lowest address, Ram ranges in map order. For each Ram range a candidate starts at its base, aligned; while it fits, the first claim it overlaps (a non-Ram range of the map, or the initrd's pages) moves it to just past that claim, aligned again:

```cpp
auto candidate = align_up(ram.base, alignment);
while (candidate.is_some() && candidate.value() < ram_end &&
       ram_end - candidate.value() >= size) {
    auto const clash = first_claim_over({
        .begin = candidate.value(),
        .end = candidate.value() + size,
    });
    if (clash.is_none()) {
        return candidate;
    }
    candidate = align_up(clash.value().end, alignment);
}
```

The walk ends because a clash always ends above the candidate (it overlaps it), so the candidate only rises; it is O(n²) in the map's at most 64 ranges, run once per boot. Overflow is the only arithmetic hazard: `convert()`'s `checked()` already refuses a range ending past 2^64, and `align_up()` returns none rather than wrap.

It refuses rather than guesses when it cannot be sure: after a conversion that dropped ranges (the dropped one could be the claim the answer sits on, the same reason the shim refuses such a map), after a failed one, for a size of 0, and for an alignment that is not a power of two.

The initrd's pages are the ones `convert()` already worked out to check the firmware's regions against them; `translate()` now keeps them in `m_initrd_pages` instead of dropping them, so both uses round the same way.

On the OpenSBI tree the answer is 0x80060000, the first page above the two regions the firmware's `/reserved-memory` names and below the shim: firmware memory is known only from the tree, as the design requires, and the test shows physload's BootMem then holds the image as `PhysImage` without a refusal.

Ruled out:
- A `floor` like the x86 function's: the device tree sits near the top of RAM, so a floor above every claim leaves no room.
- Normalising the map first (sort, subtract claims, then take the first free piece): it needs a second array as large as the map, for an answer the walk gets without one.
- Top-down placement: the x86 shim places first fit upward, and nothing downstream prefers one end.

**Reference**
Zircon's devicetree shim builds a `memalloc::Pool` from the tree (`InitMemory()`, `kernel/phys/boot-shim/devicetree.cc`: the memory nodes and reservations, with the shim's image, the tree and the ramdisk as special ranges) and allocates from it with `Pool::FindAllocatable()` (`kernel/phys/lib/memalloc/pool.cc`), first fit over free RAM, lowest address, aligned. This tree's shim has no allocator, so the same first fit runs over the converted map, stepping around claims instead of over a pool that has already subtracted them. The x86 counterpart is `bootshim::LegacyMemoryTranslator::find_scratch()`.

**Verification**
- `devicetree-smoke` (host): passes, with the new cases: the OpenSBI tree gives 0x80060000 and physload's BootMem takes it as `PhysImage`; a page steps over the firmware and the shim's last partial page to 0x80205000; a MiB steps past the initrd's pages to 0x80302000; a 2 MiB alignment gives 0x80400000; physload refuses its image over the shim's range or the initrd's last page with `AlreadyExists`, which is what the walk avoids; a fully reserved first bank moves to the second; RAM held end to end, an aligned address past 2^64, a size of 0, alignments 0 and 0x1800, an empty translator, a failed conversion and a map that dropped one range all give none.
- `scripts/format_cpp.py --check`: clean. clang-tidy (`-p out/bringup.x86` on the test, `-p out/bringup.riscv` on the shim, which includes the header): no finding in either changed file.
- x86 `ours-verify`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (152 passed).
- riscv check: `reached: shim · initrd fake.obi`, unchanged; the shim does not call it yet.

**Deliberately not done**
- Replacing the x86 version's floor with the same walk. It works for x86's map and is not on the riscv path.
- Any allocator in the shim. One placement per boot needs none.

**How it was found**
None, a new function.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which reservation in physload would refuse an image placed over the shim's own range, and with what status?
- Why does the walk always end, and what would make it loop if the jump went to the clash's start instead of its end?
- Why does a conversion that dropped a range give no answer at all, rather than the best answer from the ranges it kept?
