# x86-mm outstanding repairs

## 1. x86_tlb_global_invalidate() keeps the Global entries on a CPU without INVPCID

**Severity**
Medium: wrong on a path every boot runs; what it leaves behind is not known to have caused a failure.

**Symptom**
Latent. On a CPU without INVPCID, which includes the QEMU model the dev images use, `x86_tlb_global_invalidate()` does not invalidate Global translations. After `x86_setup_mmu()` clears PML4[0], the boot CPU may keep Global entries for low addresses it touched while PML4[0] aliased PML4[511], and those addresses lie in the user range (`USER_ASPACE_BASE` is 2 MiB, `aspace_layout.hpp:80`). A kernel user copy through such an entry would read or write the old physmap target instead of the user's page.

**Cause**
`x86_tlb_global_invalidate()` (`kernel/arch/x86/mm/mmu.cpp:24-30`) uses INVPCID type 2 when the CPU has it and otherwise `arch::tlb_invalidate_all()`, which reloads CR3 (`tlb.hpp:21-25`). Reloading CR3 flushes only non-global entries; its own comment says so. Every kernel leaf is Global (`X86Pte::make()`, `kernel/arch/x86/lib/arch/include/arch/paging/x86_pagings.hpp:87-89`), so the fallback leaves exactly the entries the callers wanted gone. The callers are `x86_init_mmu_early()` and `x86_setup_mmu()`, both on the path that tears down the boot identity mapping.

**Change**
Toggle CR4.PGE in the fallback (clear it, then set it again), which flushes every entry, global ones included, as Zircon's fallback does. QEMU's TCG most likely flushes everything on a CR3 load anyway, so the dev images may not show the difference; the fix is for real hardware.

**Reference**
Zircon `x86_tlb_global_invalidate()` (`zircon/kernel/arch/x86/mmu.cc:227-239`), which toggles PGE when INVPCID is unavailable (Intel SDM 3A, 4.10.4.1); Linux `native_flush_tlb_global()`, which does the same.

**Acceptance**
The fallback toggles PGE, checked by reading the code path on a model without INVPCID; both images boot.

## 2. The kernel shares its half with user address spaces by naming only the physmap range

**Severity**
Low: correct today by the layout, and wrong silently the day the layout changes.

**Symptom**
Latent. A user address space sees every kernel mapping only because the whole kernel address space is the one top-level slot that the physmap range happens to fall in.

**Cause**
`ArchVmAspace::init()` aliases `[PhysMap::VIRT_BASE, PhysMap::VIRT_BASE + PhysMap::SIZE)` (`arch_vm_aspace.cpp:105`). `alias_to()` copies the top-level entries covering that range, which is PML4[511] alone. The kernel image, the CPU-local window, vmalloc, stacks and MMIO are visible in user address spaces only because they sit in the same 512 GiB slot (`aspace_layout.hpp:8-9`). If the kernel range ever spans a second slot, user address spaces lose that part without a word.

**Change**
Alias the whole kernel half, `[KERNEL_ASPACE_BASE, KERNEL_ASPACE_BASE + KERNEL_ASPACE_SIZE)`, and assert at compile time that it is whole top-level slots.

**Reference**
Zircon `X86PageTableMmu::AliasKernelMappings()` (`zircon/kernel/arch/x86/mmu.cc:720-725`), which copies the whole upper half of the root.

**Acceptance**
The alias names the kernel range; both images boot and the shell's checks pass.

## 3. Every shootdown is one round of IPIs that invalidates page by page

**Severity**
Low: a performance cost, nothing wrong.

**Symptom**
Latent cost, not measured. An unmap or protect of n pages sends one round of IPIs and runs n `invlpg` on every CPU in the mask, however large n is; a clone that takes Write off several mappings sends one round per mapping.

**Cause**
`ArchVmAspace::invalidate()` is called once per `unmap()`/`protect()` and loops `invlpg` over the range. Linux switches to a full flush past a ceiling (`tlb_single_page_flush_ceiling`, 33 pages), and both Linux and Zircon batch the ranges of one operation into one shootdown.

**Change**
Measure first, with the user (CLAUDE.md, performance work): time a clone of an object mapped in N address spaces on 4 CPUs, and an unmap of a large range, with the IPI count and the time each takes. Candidates: a full flush past a ceiling; batching the ranges of one operation (Zircon's `ConsistencyManager`, Linux's `mmu_gather`); asking only CPUs that may hold the pages (the mask already narrows to the address space). The user deferred this on 2026-10-02.

**Reference**
Linux `flush_tlb_mm_range()` and `tlb_single_page_flush_ceiling`; Zircon `X86PageTableBase::ConsistencyManager`.

**Acceptance**
The measurement above, before and after, agreed with the user.
