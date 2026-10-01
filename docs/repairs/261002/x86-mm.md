# x86-mm outstanding repairs

## 1. A change to the kernel address space is invalidated on the calling CPU only

**Severity**
Medium: nothing reaches it after the APs are online today, but the first runtime unmap of a kernel range arms it, and the failure is silent memory corruption.

**Symptom**
Latent. After the APs start, no code unmaps or narrows a kernel-address-space mapping: kernel stacks are never given back (`docs/repairs/261002/mm.md` item 1), the MMIO, IDT and GDT aliases and the CPU-local window are permanent, `vfree()` runs only in the boot self-test, before the APs exist, and kernel `protect()` runs only from `reserve()` in `init_vmm()`. It becomes real as soon as one of those changes: the stack leak is fixed, or a runtime `new` above 4 KiB (which goes to vmalloc) is deleted.

**Cause**
`ArchVmAspace::invalidate()` sends the shootdown to `m_active_cpus` for a user address space, and for the kernel one only runs `invlpg` on the calling CPU. Every CPU can hold kernel translations, whatever it is running:
- Every leaf without `User` gets the Global bit (`X86Pte::make()`, `kernel/arch/x86/lib/arch/include/arch/paging/x86_pagings.hpp:87-89`), and CR4.PGE is set on every CPU, so a CR3 load does not drop them.
- The whole kernel address space is one top-level slot, PML4[511], which `alias_to()` copies into every user root (`arch_vm_aspace.cpp:105`, `page_table.cpp:73-94`), so a CPU in any process walks the kernel's own tables and caches from them.
- So the kernel address space's `m_active_cpus`, which lists only the CPUs running kernel threads, says nothing about who holds a kernel translation. Every online CPU has to be asked.

The danger is the reuse of a virtual range, not the freeing of a frame by itself:
1. CPU1 touches kernel address X (a thread's stack, a vmalloc buffer) and caches X to frame F.
2. CPU0 unmaps X, invalidates only its own TLB, and frees F.
3. The allocator is lowest-address first fit with no quarantine (`VmaSet::find_spot()`, `vm_area_or_mapping.cpp:181-290`), so the next allocation of that size gets X back, mapped to a new frame F'.
4. CPU1 uses X through its stale entry and reads and writes F, which now belongs to someone else.

An earlier version of this item said some kernel unmaps run with interrupts off. None does once more than one CPU is online: every kernel unmap holds a sleepable mutex (vmalloc's `LOCK`, the address space's lock), and the interrupts-off ones (the vmalloc self-test, `reserve()`) all run on the boot CPU before the APs exist. They still matter in one way: `IpiService::sync_exec()` asserts interrupts are on before it notices it has no one else to ask.

**Change**
Two shapes, as the references show:
- Zircon's: invalidate a kernel range on every online CPU, synchronously, before the unmap returns, so the frames and the range are released only after every CPU has dropped the translation. In `invalidate()`, the kernel branch asks `Cpu::online_mask()` and skips `sync_exec()` when that is only this CPU (the boot window). `VmMapping::destroy()` already releases the frames and the range after the unmap, in Zircon's order.
- Linux's: free the frames at once, but keep the virtual range out of the allocator until a batched flush of every CPU has run (lazy purge). Cheaper, more machinery; a performance option to measure with the user, not the first fix.

**Reference**
Zircon `X86PageTableMmu::TlbInvalidate()` (`zircon/kernel/arch/x86/mmu.cc:382`), which targets every CPU for a global entry, and `VmMapping::DestroyLocked()`, which drops the VMO after the unmap. Linux `vfree()` (`mm/vmalloc.c`), which frees the pages before the flush, and `__purge_vmap_area_lazy()`, which runs `flush_tlb_kernel_range()` on every CPU before a freed range is handed out again.

**Acceptance**
A two-CPU check in `vmosnapshot.test` or a sibling command: one CPU reads a kernel mapping and keeps running without loading CR3 (preemption off); the other unmaps it and maps a different object at the same address; the first then reads the address and sees the new object's bytes. It fails today on the dbg image and passes after the fix. A capturing access cannot be used here: a ring-0 fault on a kernel address is never handed back.

## 2. x86_tlb_global_invalidate() keeps the Global entries on a CPU without INVPCID

**Severity**
Medium: wrong on a path every boot runs; what it leaves behind is not known to have caused a failure.

**Symptom**
Latent. On a CPU without INVPCID, which includes the QEMU model the dev images use, `x86_tlb_global_invalidate()` does not invalidate Global translations. After `x86_setup_mmu()` clears PML4[0], the boot CPU may keep Global entries for low addresses it touched while PML4[0] aliased PML4[511], and those addresses lie in the user range (`USER_ASPACE_BASE` is 2 MiB, `aspace_layout.hpp:80`). A kernel user copy through such an entry would read or write the old physmap target instead of the user's page.

**Cause**
`x86_tlb_global_invalidate()` (`kernel/arch/x86/mm/mmu.cpp:24-30`) uses INVPCID type 2 when the CPU has it and otherwise `arch::tlb_invalidate_all()`, which reloads CR3 (`tlb.hpp:21-25`). Reloading CR3 flushes only non-global entries; its own comment says so. Every kernel leaf is Global (item 1), so the fallback leaves exactly the entries the callers wanted gone. The callers are `x86_init_mmu_early()` and `x86_setup_mmu()`, both on the path that tears down the boot identity mapping.

**Change**
Toggle CR4.PGE in the fallback (clear it, then set it again), which flushes every entry, global ones included, as Zircon's fallback does. QEMU's TCG most likely flushes everything on a CR3 load anyway, so the dev images may not show the difference; the fix is for real hardware.

**Reference**
Zircon `x86_tlb_global_invalidate()` (`zircon/kernel/arch/x86/mmu.cc:227-239`), which toggles PGE when INVPCID is unavailable (Intel SDM 3A, 4.10.4.1); Linux `native_flush_tlb_global()`, which does the same.

**Acceptance**
The fallback toggles PGE, checked by reading the code path on a model without INVPCID; both images boot.

## 3. The kernel shares its half with user address spaces by naming only the physmap range

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

## 4. Every shootdown is one round of IPIs that invalidates page by page

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
