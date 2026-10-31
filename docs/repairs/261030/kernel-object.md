# kernel-object outstanding repairs

## 1. A physical VMO has no size cap, and mapping one builds a 4 KiB page-table entry per page

**Severity**
Medium: a denial of service by a component that already holds the MmIo root, not a false grant, and not reached by any component of the images today.

**Symptom**
Latent, cannot be triggered today. `vmo_create_physical(root, 0x80_0000_0000, 512 GiB)` succeeds, since that range is above RAM and above the kernel's devices, and a `vma_map` of the whole window then populates it: about 134 million 4 KiB entries, which need about a gibibyte of page-table pages taken from the kernel's own pool under the address space lock. If the map fails for want of memory, the tables already built stay until the process dies, because unmapping never frees a table. At exit, tearing the mapping down runs one `invlpg` per page in an IPI on every active CPU.

**Cause**
`VmoDispatcher::create_physical()` and `VmObjectPhysical::create()` refuse a window of 0 bytes, a misaligned one and one that wraps, and no more. `VmObjectPaged` has a `MAX_SIZE`; the physical object has none. `VmMapping::map_physical()` appends every page to a 32-page coalescer and `map_frames()` makes only 4 KiB leaves, so a large aligned window never gets a 2 MiB leaf. Forcing `Populate` for a physical VMO (step 3 of `.agents/tasks/driver-capabilities/PLAN.md`) is correct, since no fault could fill the window later, and it made the cost reachable in one call. A paged VMO of that size would need real backing memory first; a physical one costs the caller nothing.

**Change**
Options, none chosen:
- a `MAX_SIZE` on `VmObjectPhysical`, which bounds one window but not a window mapped again and again;
- 2 MiB leaves for aligned runs of the window in `map_physical()`, which cuts the cost of a large window by 512;
- a limit on the page-table pages one address space may hold, which is the control for the whole class and also covers the same amplification by a paged VMO mapped many times.
The size of a real window is small (a PCI BAR is megabytes), so the cap can be generous. Measure the table pages and the teardown time of a 1 GiB window first if the 2 MiB leaf is taken.

**Reference**
Zircon bounds nothing here either: `VmObjectPhysical::Create` checks the alignment and the wrap (`kernel/vm/vm_object_physical.cc`). None for the page-table limit, this project's own design.

**Acceptance**
A `vmo_create_physical` or a `vma_map` of a window past the cap answers `OutOfRange` or `NoResource`, a window of the largest allowed size maps and unmaps in bounded time, and a test in routing_checks pins both.

## 2. A user mapping of a physical VMO is write-back, not uncached

**Severity**
Medium: wrong on real hardware, where a CPU may serve a read of a device register from its cache and combine or reorder writes; not reached on QEMU, where the registers are emulated and the cache does not sit in front of them.

**Symptom**
Latent, cannot be triggered on QEMU. On a real machine a driver that polls a status register through a physical mapping could read a stale value.

**Cause**
`VmaDispatcher::map()` builds the mapping's `MmuFlags` from the options alone. `MmuFlags::Discache` exists, `arch_mmu_flags.hpp` turns it into the PCD bit, and the kernel's own mappings of the local APIC and the IO-APIC pass it; a user mapping of a physical VMO does not. `vmo_create_physical`'s documentation says the mapping is cacheable. The first draft of the driver-capabilities design wrongly said the page tables had no cache attribute, which would have made this look larger than it is.

**Change**
One flag, `mmuf |= mm::MmuFlags::Discache`, next to the forced `Populate` for a physical VMO in `VmaDispatcher::map()`; the documentation of the call changes with it. Recommended now, since the reference does it by default; the user decides, as it is a departure either way. Write-combining, which a framebuffer wants, needs the PAT and is a different item.

**Reference**
Zircon `VmObjectPhysical::Create()` makes every physical VMO uncached by default (`SetMappingCachePolicy(ARCH_MMU_FLAG_UNCACHED)`, `kernel/vm/vm_object_physical.cc:140-160`: "Physical VMOs should default to uncached access"), and `sys_vmo_set_cache_policy()` (`kernel/lib/syscalls/vmo.rs:241`) changes it before the VMO is mapped: the policy belongs to the VMO, not to the mapping. Leaving ours write-back is the departure from the reference, so the recommendation is to take the flag now.

**Acceptance**
A mapping of a physical VMO has the PCD bit set in its page-table entries, shown by a kernel test that walks the entry, and the routing_checks read of the host bridge's vendor id still passes.

## 3. No resource can be narrowed, so a driver holds the whole MmIo root

**Severity**
Medium: every component routed `ours.resource.Mmio` can map any device on the machine, not only its own.

**Symptom**
Latent, cannot be triggered today: only routing_checks is routed the protocol.

**Cause**
The root hands out a duplicate of its whole resource, and nothing mints a child of a smaller range. The creation function takes a range, but only the kernel calls it.

**Change**
A `resource_create(parent, base, size, name, out)` syscall: the child has the parent's kind, its range lies inside the parent's (OutOfRange otherwise), a System slot cannot be split. A PCI bus component can then hand each driver the one BAR it owns. Until then the root's grant is all or nothing.

**Reference**
Zircon `sys_resource_create` (`kernel/lib/syscalls/resource.rs:39`).

**Acceptance**
A component holding a narrowed MmIo resource maps its own range and is refused (OutOfRange) an address outside it, in a routing_checks test.

## 4. Two drivers can hold the same MmIo range

**Severity**
Low: nothing in the images maps device registers yet.

**Symptom**
Latent, cannot be triggered today. Two components routed the protocol can map the same BAR.

**Cause**
`ResourceDispatcher::create()` keeps no record of the ranges it has minted. The `Storage` parameter and the flag bit `0x00010000` are kept for it.

**Change**
Track overlap, and an exclusive flag, in `Storage`, as the per-kind region allocator does in Zircon. This does not decide what RAM is; the deny list of step 1 does.

**Reference**
Zircon `ResourceDispatcher::create` (`kernel/object/resource_dispatcher.rs:329`), with `ZX_RSRC_FLAG_EXCLUSIVE`.

**Acceptance**
A second exclusive resource over a range already minted exclusively is refused, in a kernel test.
