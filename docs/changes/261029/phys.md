# phys changes

## The boot shim maps the 4 GiB it builds entries for, not 1 (2026-10-29)

**Files**
- `kernel/arch/x86/phys/start64.S` — links every page of the page directory into the PDP, not only the first; the page count comes from one constant.
- `kernel/arch/x86/phys/include/ours/phys/arch_paging.hpp` — `X86_BOOT_IDENTITY_MAP_SIZE`, the size of the shim's identity map, readable from assembly.

**Symptom**
No machine with more than 1 GiB of RAM booted. `MEM=2048` ended in `QEMU exited (rc 0) before the shell finished -- reset or triple fault? ... reached: phys`, the log stopping at physload's `crashlog: reserved [0x4000000, 0x4004000)`, with or without NUMA. `MEM=1024` and below passed. After the change `MEM=2048` boots both images.

**Cause**
x86-64 translates an address through four tables. CR3 points at the PML4; a PML4 entry points at a PDP table, each of whose 512 entries covers 1 GiB; a PDP entry points at a page directory (PD), each of whose 512 entries covers 2 MiB -- and with the page-size bit set, a PD entry maps those 2 MiB directly. So one page of PD maps exactly 1 GiB, and it is reachable only through its own PDP entry.

`start64.S` built four pages of PD, 2048 entries mapping [0, 4 GiB), and then wrote one PDP entry:

```text
    // PDP[0] = PD | X86_KERNEL_PD_FLAGS
    lea rva(PD)(%ebx), %eax
    or $X86_KERNEL_PD_FLAGS, %eax
    mov %eax, rva(PDP)(%ebx)
```

The other three pages were filled in and pointed at by nothing. The hardware never reads a table no entry points to, so the shim mapped 1 GiB, not 4, and `kernel/arch/x86/phys/BUILD.gn`'s description of "an identity-mapped 4 GiB of 2 MiB pages" was true of the entries and false of the mapping.

Nothing fails until something touches memory above 1 GiB before physload has page tables of its own, and the first thing to do that is physload building those page tables. `Aspace::alloc_page_table()` takes them top-down from the highest usable address and zeroes each one at once, before its log line:

```cpp
    if (phys) {
        std::memset(reinterpret_cast<void *>(phys), 0, size);
    }

    log::debug("Allocate page table (p:0x{:X}, s:0x{:X})", phys, size);
```

The first allocation tracks the top of RAM -- `0x1FEBB000` at 512M, `0x3FEBB000` at 1024M, the latter 1.3 MiB under the cliff. One more megabyte put it above `0x40000000`, the `memset` faulted, and with no IDT yet the fault became a triple fault, which `-no-reboot` turns into QEMU exiting. That is why the log stopped between the crashlog line and the `Allocate page table` line that never came.

**Change**
One PDP entry per page of PD:

```text
    lea rva(PD)(%ebx), %eax
    or $X86_KERNEL_PD_FLAGS, %eax
    lea rva(PDP)(%ebx), %edi
    movl $BOOT_PD_PAGES, %ecx
1:
    mov %eax, (%edi)
    add $PAGE_SIZE, %eax
    add $8, %edi
    loop 1b
```

Only the low half of each 8-byte entry is written: the high half is already zero from the BSS clear at the top of the shim, and PD is part of the shim's own image, which is loaded below 4 GiB.

The size of the map was written three times -- `.skip (PAGE_SIZE << 2)`, `X86_MAX_PTES << 2`, and the comment in `BUILD.gn` -- and the PDP write was the place that disagreed with them. It is now one constant, `X86_BOOT_IDENTITY_MAP_SIZE`, in the phys paging header, with `BOOT_PD_PAGES` derived from it in `start64.S`. A `#define`, because the assembly reads it, and outside the header's C++ part, which is now guarded from the assembler. physload is about to read the same constant (the next change), and the point of a single definition is that the shim and its consumer cannot disagree about it again.

**Reference**
None for the bug itself; it is a missing loop. The shape of the early mapping differs from Linux's on purpose: `arch/x86/kernel/head_64.S` maps only what the decompressed kernel needs, through `level2_kernel_pgt`, and switches to a full direct map in `init_mem_mapping()` before touching high memory, building those tables from a pool it already has mapped. This shim maps a fixed 4 GiB instead, which is enough only if nothing written before physload installs its own tables lies above it. That is the second half of the repair, still open: see `docs/repairs/261029/phys.md`.

**Verification**
- `MEM=2048`, both images: dbg 44 passed 4 skipped, rel 41 passed 8 skipped 6/6 required (both failed in physload before).
- `MEM=1024`, the old ceiling, both images: pass.
- `out/bringup.x86` at the default 512M: build ok, dbg 44 passed, rel 41 passed 6/6, host-tests 151 passed; `pmnode.test` 21 checks, `pmzone.test` 3 checks, both ok.
- `NUMA=1` and `NUMA=33` on `out/numa.x86`, both images: pass.
- `MEM=4096` still fails in physload, as expected and for a different reason: the root table now lands at `0x7FEDB000`, inside the map, and the next table is taken from the top of RAM, which on that machine is at 6 GiB -- QEMU's q35 puts RAM beyond about 2.75 GiB above the 4 GiB PCI hole. That is the open half of the repair.
- clang-tidy over `kernel/arch/x86/phys/aspace.cpp`, which includes the changed header: nothing in either file.

**Deliberately not done**
- Bounding physload's page-table allocations to the shim's map, which is what makes RAM above 4 GiB boot. It is a separate change to a separate component, verified separately.
- `LOWER_MAPPING_BASE`, `LOWER_MAPPING_SIZE`, `UPPER_MAPPING_BASE` and `UPPER_MAPPING_SIZE` in the same header have no reader anywhere in the tree; `LOWER_MAPPING_SIZE = 1_gb` reads like a description of this map and is not used as one. Left alone, since removing dead names is not this fix.

**How it was found**
While building a QEMU machine with more proximity domains than `MAX_NODES` (`959fe947`): the first attempt gave it 2112M and it never reached the kernel. Bisecting memory with NUMA held fixed, and NUMA with memory held fixed, put the boundary between 1024M and 1056M on every machine shape, and the first page table's address on each passing boot (`0x1FEBB000`, `0x20EBB000`, `0x3FEBB000`) showed it tracking the top of RAM toward `0x40000000`. Reading `start64.S` with "one PDP entry per GiB" in mind found the single write.

**The user's prediction**
None -- homework is paused.

**Three questions you should be able to answer**
1. The shim filled 2048 page-directory entries correctly and still mapped only 1 GiB. Why does the hardware never see the other 1536?
2. Why did the boot die at physload's first page-table allocation, rather than at some earlier access above 1 GiB, and why did the log stop one line short of naming the address?
3. After this change, why does a 4096M machine still fail in physload when a 2048M one does not?

## physload takes its page tables from inside the shim's map, so a machine with RAM above 4 GiB gets through phys (2026-10-29)

**Files**
- `kernel/arch/x86/phys/aspace.cpp` — the page tables `map_usable_ram()` allocates are bounded by the shim's identity map instead of not at all.

**Symptom**
With the shim mapping its full 4 GiB (the entry above), `MEM=4096` still triple-faulted in physload, one step later than before: the root table was allocated at `0x7FEDB000`, inside the map, and the boot died at the next table. Any machine with RAM above 4 GiB did the same -- on QEMU's q35 that is anything over 2816M, which it splits between the space below the 4 GiB PCI hole and the space above it.

**Cause**
`arch_setup_aspace()` builds physload's page tables in two phases, and only the first was bounded:

```cpp
    aspace.set_allocation_bounds(0, 4_gb);
    aspace.init();

    // Cancel the allocation bounds of page table and create identity map of RAM under ARCH_PHYSMAP_SIZE.
    aspace.set_allocation_bounds(0, 0);
    aspace.map_usable_ram(ARCH_PHYSMAP_SIZE);
```

The root is held below 4 GiB because an application processor coming up loads it into CR3 while still in protected mode, where CR3 is 32 bits wide. That is a real constraint, and it is about the root alone. With the bounds cancelled, `Aspace::alloc_page_table()` falls back to the whole of RAM and takes each further table top-down from the highest usable address -- 6 GiB on a 4096M q35 machine -- and zeroes it there.

But every table built here is written before physload's own page tables are installed, which happens only at the end of `arch_setup_aspace()`. Until then the live mapping is the shim's, so a table is writable only if it lies inside the shim's identity map. That second constraint applies to every table, and nothing expressed it: the code bounded the one table that needed a bound for a different reason, then took the bound away from the rest. On a machine whose RAM ends inside the map the omission was invisible, because the top of RAM was mapped anyway.

**Change**
```cpp
    aspace.set_allocation_bounds(0, X86_BOOT_IDENTITY_MAP_SIZE);
    aspace.map_usable_ram(ARCH_PHYSMAP_SIZE);
```

The bound is the constant `start64.S` builds its map from (the entry above), so the shim and physload cannot disagree about it. It stays two calls although both bounds are 4 GiB today: they are different facts, and widening the shim's map would not let the root move above 4 GiB. The bound also covers the VGA-hole mapping that follows, which allocates the same way and is written through the same map.

The bound is not lifted after `install()`. physload's own map covers all usable RAM by then, so tables could come from anywhere, but nothing physload builds afterwards is large enough to need memory above 4 GiB, and a bound that is always on cannot be forgotten in the wrong phase.

A machine with no free RAM below the bound fails the allocation, which the paging code reports as `MapError::NoMem` and `map_usable_ram()` turns into a panic naming it, rather than writing to an unmapped address.

**Reference**
Linux's `alloc_low_pages()` in `arch/x86/mm/init.c`, which supplies the page tables `init_mem_mapping()` builds the direct map with, takes them from memblock only between `min_pfn_mapped` and `max_pfn_mapped` -- memory already mapped -- and `init_mem_mapping()` maps top-down so that each newly mapped range can host the tables for the next. The rule is the same: never allocate a table you cannot yet write.

**Verification**
- `out/bringup.x86` at 512M: build ok, dbg 44 passed, rel 41 passed 6/6, host-tests 151 passed; `pmnode.test` 21 checks, `pmzone.test` 3 checks, both ok.
- `MEM=1024`, `2048`, `2560`, both images: pass.
- `NUMA=1` and `NUMA=33` on `out/numa.x86`, both images: pass.
- `MEM=3072`, `4096` and `8192`, both images: physload completes and the kernel boots most of the way -- at 8192M it releases 8051 MiB to the buddy allocator, sets up the zones, vmalloc, the local APIC, the HPET and the timers -- and then fails an `ASSERT` while preparing to wake the application processors. That is a separate bug, in the kernel's SMP bring-up, reachable only now that such a machine gets through phys; it is recorded in `docs/repairs/261029/x86-kernel.md`. The phys repair item's acceptance ("`MEM=4096` and `MEM=8192` boot both images to the end marker") is therefore not met by this change alone, and the item is closed because its own cause is gone, not because those machines boot.
- clang-tidy over `kernel/arch/x86/phys/aspace.cpp`: nothing in the changed file or the header.

**Deliberately not done**
- The SMP bring-up failure above, which belongs to a different component and needs its own diagnosis.

**How it was found**
Predicted from the first half's own verification: on `MEM=4096` the debug log showed the root table allocated below 4 GiB and then nothing, which is what an unbounded allocation from a 6 GiB top of RAM looks like.

**The user's prediction**
None -- homework is paused.

**Three questions you should be able to answer**
1. The root table and the other page tables were both bounded to 4 GiB after this change. Give the two different reasons, and say which one would change if the shim mapped 8 GiB.
2. Why was the missing bound harmless on every machine whose RAM ended below 4 GiB?
3. Why is it enough to bound the tables, and not also the kernel image and the modules physload loads afterwards from the top of RAM?
