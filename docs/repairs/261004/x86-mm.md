# x86-mm outstanding repairs

## 1. physboot's identity-map page tables are unhooked at boot and never freed

**Severity**
Low: a few frames leaked once per boot, out of the 12 page-table frames (48 KiB) the stamp counts. Nothing reads them, so nothing goes wrong.

**Symptom**
Latent, cannot be observed without walking the tables. After boot, the page-table frames that physboot built for the identity map of low RAM stay Role::Mmu, Reserved, and counted in the node's and zone's reserved counts. No root points at them.

**Cause**
physboot builds one root (`arch_setup_aspace()`, `kernel/arch/x86/phys/aspace.cpp`). Under it are the identity map of usable RAM (`map_usable_ram()`) and the VGA hole, both under `pgd[0]`, besides the physmap and the kernel image. Every table it allocates is a `bootmem::MemoryType::PageTable` reservation (`kernel/phys/aspace.cpp`), which `stamp_reserved_frames()` marks Mmu and Reserved and counts. The kernel then drops the identity map:
- `x86_init_mmu_early()` (`kernel/arch/x86/mm/mmu.cpp`) sets `pgd[0] = pgd[511]`;
- `x86_setup_mmu()` sets `pgd[0] = 0`.

The PDPT that `pgd[0]` pointed at, and the tables under it, are now unreachable, and no code frees them. `unwire_frame()` would refuse them anyway: it asserts role None.

**Change**
After `x86_setup_mmu()`, walk the subtree the old `pgd[0]` pointed at (save the entry before overwriting it). For each table frame, take it off the reserved books and free it. That needs an un-wire path that accepts Role::Mmu, which Linux's `free_pagetable()` (`arch/x86/mm/init_64.c`) does by testing `PageReserved()`. Zircon has nothing to free here: physboot's tables become the kernel aspace's own pages at hand-off (`X86ArchVmAspace::HandoffPageTablesFromPhysboot()`) and are freed by its ordinary unmap path. Measure first how many frames the subtree holds; if it is one or two, recording the leak may be the better answer.

**Reference**
Linux `free_pagetable()` and `remove_pagetable()` in `arch/x86/mm/init_64.c`; Zircon `X86ArchVmAspace::HandoffPageTablesFromPhysboot()` in `zircon/kernel/arch/x86/mmu.cc`.

**Acceptance**
- The frames under the old `pgd[0]` return to the buddy, and the reserved counts drop by exactly that many.
- `ours-verify` passes, including SMP boots: the AP trampoline must not still depend on the identity map when it is freed.
