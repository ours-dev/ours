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

## 2. No resource can be narrowed, so a driver holds the whole MmIo root

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

## 3. The stub IOMMU isolates nothing, so a driver's device can reach all of memory

**Severity**
Medium: a pin keeps a page where it is and stops the kernel reusing it; it does not stop a device reading or writing any other address. Today the drivers are trusted components and the only devices are QEMU's, so nothing exploits it.

**Symptom**
Latent, cannot be triggered today. A driver that programs its device with an address it never pinned, or with a pinned address and a longer length, makes the device read or write memory the BTI knows nothing about, including the kernel's. The `PermRead` and `PermWrite` bits of `bti_pin` are checked against the VMO's rights and then nothing enforces them on the device.

**Cause**
`IommuType::Stub` is the only type: no translation hardware is driven, so a device address is a physical address and a BTI is bookkeeping (`IommuDispatcher`). That is the design for the first driver, chosen so that the interface is the real one and a hardware IOMMU is a new type behind it.

**Change**
A type for the machine's IOMMU (VT-d needs the ACPI DMAR table, parsed in user space or by the kernel) whose `pin` maps the range into the device's own address space with the permissions of `BtiPinOptions`, whose `query_address` answers device addresses and not physical ones, and whose fault state also covers a device fault. Until then, a device that must not be trusted is not given to a driver.

**Reference**
Zircon `kernel/dev/iommu/arm_smmu` for the shape of a driver behind `iommu::Bti`; the stub is `kernel/dev/iommu/stub`. The Intel driver is not in the checkout read.

**Acceptance**
A device address outside the ranges pinned for a BTI faults, in a test on a machine with an IOMMU (QEMU `-device intel-iommu`), and `PermRead` without `PermWrite` refuses a device write.

## 4. A driver that dies with memory pinned leaks it, and a restarted driver cannot release it

**Severity**
Medium: the kernel stays correct and loses memory, one buffer set per crash of a driver while its device was doing DMA.

**Symptom**
Latent, cannot be triggered today. A driver process dies with a pin outstanding; its handles close; the token's last handle going quarantines the pin, the BTI's last handle going orphans the BTI, and the quarantine is leaked (the destructor keeps the pages pinned and the VMO alive for good). A restarted driver makes a new BTI, which has no quarantine to release.

**Cause**
The quarantine belongs to a BTI object, and a BTI object is made fresh by every `bti_create`; nothing keys it by (IOMMU, id). If no process outside the driver holds a handle to the BTI, there is nobody left to say that the device has been reset.

**Change**
Two options. (a) No kernel change: whoever starts the driver keeps a duplicate of its BTI handle and gives the restarted driver a duplicate again; the BTI then lives across the crash, the new driver resets its device, and calls `bti_release_quarantine`. That is a rule for the component framework and for `init` (step 6 and the hand-over of step 9). (b) The kernel keeps one BTI per (IOMMU, id) and hands it to a later `bti_create`, which makes a driver's crash invisible to the object but needs a place to hold it. Recommended: (a), written into the driver's contract.

**Reference**
Zircon's stub IOMMU creates a fresh BTI on every `CreateBti` (`kernel/dev/iommu/stub/stub_iommu.cc`), and its header says the quarantine is released by the restarted driver; there the platform bus holds the handle across the restart.

**Acceptance**
A test that kills a driver component with a pin outstanding, restarts it, and shows the new instance releasing the quarantine and pinning again.

## 5. Pinned memory is not limited, so a driver can pin all of RAM

**Severity**
Medium: a denial of service by a component that holds an IOMMU and a VMO, not a false grant.

**Symptom**
Latent, cannot be triggered today. `bti_pin` of a large VMO pins every page of it, a page can be pinned 255 times, and pinned pages cannot be reclaimed; nothing bounds the total. A leaked quarantine (item 4) is permanent.

**Cause**
`BtiDispatcher::pin()` calls `VmObject::pin_range()`, which counts per page and nothing in total, and the BTI has no limit of its own.

**Change**
A limit on the pages pinned through one BTI, and one on the system, with `NoResource` past it; the first is cheap (the BTI knows its counts), the second is a counter in the page queues. Decide the numbers when a real driver has a measured need.

**Reference**
None, this project's own design.

**Acceptance**
A `bti_pin` past the limit answers `NoResource`, leaves nothing pinned, and the count goes back down on unpin and on release.

## 6. Two drivers can hold the same MmIo range

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
