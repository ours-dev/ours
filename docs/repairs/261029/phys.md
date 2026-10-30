# phys outstanding repairs

Found on 2026-10-29 while building a QEMU machine with more proximity domains than the kernel can hold. Nothing to do with NUMA: the machine simply needed more memory than the four-node one, and no boot with more than 1 GiB of RAM survived physload. The first half of the cause -- the boot shim mapping 1 GiB where it built entries for 4 -- was fixed the same day; see `docs/changes/261029/phys.md`. What is left is the half that makes the shim's mapping size matter at all.

## 1. physload takes its page tables from the top of RAM, outside the window the boot shim maps, so a machine with RAM above 4 GiB triple-faults

**Severity**
High. Every machine with RAM above 4 GiB fails to boot before the kernel gets control, which is every real machine with more than about 3 GiB. Invisible on the images this repo boots, all of which are given 1 GiB or less.

**Symptom**
`MEM=4096 python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/<dir> --only dbg` reports `QEMU exited (rc 0) before the shell finished -- reset or triple fault? ... reached: phys`. QEMU's q35 places RAM beyond about 2.75 GiB above the 4 GiB PCI hole, so at 4096M the top of RAM is at 6 GiB. The debug log shows the root table allocated inside the map, then nothing:

```text
[info] crashlog: reserved [0x4000000, 0x4004000)
[debug] Allocate page table (p:0x7FEDB000, s:0x1000)
[debug] Map identically (0x0, 0xA0000)
```

**Cause**
`arch_setup_aspace()` in `kernel/arch/x86/phys/aspace.cpp` bounds only the root table's allocation, and then cancels the bounds:

```cpp
    aspace.set_allocation_bounds(0, 4_gb);
    aspace.init();

    // Cancel the allocation bounds of page table and create identity map of RAM under ARCH_PHYSMAP_SIZE.
    aspace.set_allocation_bounds(0, 0);
    aspace.map_usable_ram(ARCH_PHYSMAP_SIZE);
```

The root is held below 4 GiB for a reason of its own: a CPU coming up in protected mode loads a 32-bit CR3. With the bounds cancelled, `Aspace::alloc_page_table()` takes every other table from anywhere in RAM, top-down, and zeroes it at once -- through the shim's identity map, because physload's own tables are not installed until `arch_setup_aspace()` ends. Any table above the shim's map faults with no handler. The constraint that a table be reachable through the mapping that is live while it is written exists, and nothing expresses it.

**Change**
Keep the second phase bounded too, by the shim's map rather than by the CPU's CR3 width: `set_allocation_bounds(0, X86_BOOT_IDENTITY_MAP_SIZE)` in place of `(0, 0)`. The constant is the one `start64.S` builds its map from, so the two cannot drift. Both bounds happen to be 4 GiB today and must stay two calls, each with its reason, because they are different facts: widening the shim's map would not let the root move.

A machine with no free RAM below the bound then fails an allocation, which `map_usable_ram()` already turns into a panic that names it, rather than writing to an unmapped address.

**Reference**
Linux's `alloc_low_pages()` in `arch/x86/mm/init.c`, which supplies the page tables `init_mem_mapping()` builds the direct map with, allocates from memblock only between `min_pfn_mapped` and `max_pfn_mapped` -- memory already mapped -- and `init_mem_mapping()` maps top-down so that each newly mapped range can host the tables for the next. Never allocate a table you cannot yet write.

**Acceptance**
`MEM=4096` and `MEM=8192` boot both images to the end marker, and `MEM=2048`, `MEM=1024`, the default 512M and the NUMA machines keep passing.
