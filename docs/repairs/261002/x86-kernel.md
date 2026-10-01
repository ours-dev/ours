# x86-kernel outstanding repairs

## 1. The AP trampoline's identity mappings stay in the kernel page table after their frames are freed

**Severity**
Medium: latent, but what it leaves is a live supervisor mapping onto freed frames and onto the kernel GDT.

**Symptom**
Latent. After the APs are up, the kernel address space still maps, at virtual address equal to physical address, the three-page trampoline buffer below 64 KiB and the page holding the kernel GDT, read-write-execute and Global. The buffer's frames have been freed to the allocator meanwhile, so a stray kernel access through those addresses writes into whoever owns them next. The GDT's identity address lies in the user range whenever the GDT is physically at or above 2 MiB: an AP that still caches that Global supervisor entry would satisfy a kernel user copy to the same address in a user process from the GDT instead of the user's page.

**Cause**
`setup_bootstrap()` maps `BOOTSTRAP16_BUFFER_BASE` and `gdt_base` 1:1 into the kernel address space (`kernel/arch/x86/kernel/bootstrap.cpp:105-113`), every AP walks them during bring-up (`bootstrap16.S`), and nothing unmaps them. `free_bootstrap16_buffer()` (`bootstrap.cpp:133-140`) frees the buffer's frames with the mapping still in place.

**Change**
Make the identity mappings in an address space of their own that only the trampoline uses, and destroy it once every AP is running, before the buffer's frames are freed, as Zircon does. Failing that, unmap both ranges after AP bring-up with a global invalidation on every CPU (which needs `docs/repairs/261002/x86-mm.md` item 2, since today's "global" invalidation keeps Global entries).

**Reference**
Zircon `x86_bootstrap16_acquire()` and `x86_bootstrap16_release()` (`zircon/kernel/arch/x86/bootstrap16.cc:62-213`), which build the identity mappings in a separate `LowKernel` `VmAspace` and release it after use.

**Acceptance**
After boot, a query of the kernel address space at `BOOTSTRAP16_BUFFER_BASE` and at the GDT's physical address finds nothing; both images boot with all CPUs.

## 2. The trampoline's GDT copy is placed by pointer arithmetic on the wrong type

**Severity**
Low: on a path no boot takes today.

**Symptom**
Latent. When the kernel GDT lies above 4 GiB, the copy meant to sit just after the trampoline data is placed far beyond it, and the ASSERT that it fits in the buffer fires.

**Cause**
`bootstrap_data + sizeof(*bootstrap_data)` (`bootstrap.cpp:71`) advances a `BootstrapData *` by `sizeof(BootstrapData)` elements, not bytes. The normal path never reaches it: the GDT is page-aligned in the kernel image (`gdt.S`), below 4 GiB. Even with the arithmetic fixed the copy path would still fail, because `gdt_start` is only 16-byte aligned and the identity `map()` at `bootstrap.cpp:110` needs a page-aligned address.

**Change**
Compute the address in bytes and page-align the copy, or drop the copy path if the GDT can be guaranteed below 4 GiB.

**Reference**
None, this project's own code.

**Acceptance**
A boot with the GDT forced above 4 GiB (or a host test of the address computation) reaches every AP.
