# driver-capabilities: the kernel side of a userspace PCI driver, run through first

**Date**: 2026-10-30
**Status**: Proposed

## Request

"先跑通吧": get the path working end to end before widening it. The requester is the userspace virtio-blk driver on `exp/fs-service` (`.agents/tasks/storage-layer/KERNEL-REQUESTS.md`). The scope ends where that driver's acceptance flow can run: enumerate PCI through ECAM, map the BARs, build rings in DMA memory, bind a PCI interrupt, read block 0. The narrowing syscall, ACPI and MSI-X are out of scope and go to `docs/repairs/`.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Physical VMO entry point | A: flag on `vmo_create`; B: `vmo_create_physical` | B | None | B, 2026-10-30 |
| Gating resources | MmIo, and a System slot (IOMMU) for the BTI model; or a new Dma kind | MmIo and a System slot | None | MmIo and a System slot, 2026-10-30 |
| R1 transport | A: open 0xCF9; C: `pci_cfg_pio_rw`; D: ECAM through MmIo | D | None | D, 2026-10-30 |
| R4 route | Shortcut; standard (userspace ACPI); MSI-X | Shortcut | None | Shortcut, 2026-10-30 |
| MmIo denial pattern | A deny list checked even for the root; or only a region allocator | Deny list | None | Deny list, 2026-10-30. Zircon uses the same deny list (`RootResourceFilter`), so this is no departure; the question as first put said it was |
| Cache attribute of a physical mapping | Uncached from the first version; write-back and later | Uncached | None | Uncached, 2026-10-30 |
| Exclusive MmIo ranges | Track overlap and an exclusive flag with a region allocator; defer | Defer | None | Open: not needed for the first run |
| DMA address and gate | Dma resource and info topic; BTI object | Resource and info topic | None | The BTI model as Zircon, 2026-10-30: the user's choice over the recommendation |

## Steps

Each step is one commit and is introduced to the user before it starts.

1. The `MmIo` resource kind. `ResourceDispatcher::create` accepts it, the root covers the physical address space, and `validate_ranged_resource()` refuses a range that touches RAM (a copy of the memory manager's map, `kernel_ram()`) or MMIO the kernel drives (`kernel_mmio()`, beside `kernel_ioports()`). Verified by `resource_test` and `ours-verify`.
2. `ours.resource.Mmio` as a built-in served by the root, so a component can use it from its manifest: the kernel's userboot mints the MmIo root, the user-mode userboot passes it on, the root serves it with a ResourceServer, and routing_checks gets it from root.cm. Verified by a boot where routing_checks receives it and object_get_info says it is the MmIo root.
3. `vmo_create_physical(resource, paddr, size, out)` and mapping it. Verified by a test that maps QEMU's ECAM window (0xE0000000 under edk2) and reads the host bridge's vendor id.
4. Range pinning in mm: a pin of a range of a paged VMO that can be undone and is counted, and that the pages' reclaim, decommit, resize and clone respect. A kernel test shows each of them refused while a pin is held and allowed after it is undone.
5. The IOMMU stub, the BTI and the PMT in `kernel/object`, and the calls `iommu_create`, `bti_create`, `bti_pin`, `pmt_unpin` and `bti_release_quarantine`, with the quarantine. The `IOMMU` System slot is checked by `iommu_create`. Verified by a kernel test of the objects, and by syscall-level checks of every refusal.
6. The IOMMU slot reaches a component: the kernel's userboot mints it, the user-mode userboot passes it on, the root serves `ours.resource.Iommu`, and root.cm offers it to routing_checks. Verified as step 2 was, with a mutation.
7. `vmo_create_contiguous(bti, size, alignment_log2, out)` and `VmObjectPaged::create_contiguous()`. Verified in routing_checks end to end: iommu, BTI, a contiguous VMO, a pin, addresses that are page aligned and contiguous, an unpin, and a PMT dropped without one released through `bti_release_quarantine`.
8. IO-APIC pins past the ISA range registered as lines. Verified by `interrupt_create` on a PCI GSI and an interrupt arriving from a virtio device.
9. Hand the interface list back to the `exp/fs-service` side, and record the deferred items in `docs/repairs/`.

## Files

- `kernel/object/resource_dispatcher.cpp` and its header: the MmIo kind and its validation (step 1).
- `kernel/platform/` (pc, arm, riscv) and `kernel/platform/include/ours/platform/`: `kernel_mmio()` (step 1).
- `kernel/commands/resource_test.cpp`: the MmIo cases (step 1).
- `system/service/init/builtins.json` and `resource_server.*`: the built-in (step 2).
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/vmo.cpp`: `vmo_create_physical` (step 3), `vmo_create_contiguous` (step 7).
- `kernel/mm/vm_cow_pages.*`, `vm_object_paged.*`: range pinning (step 4) and contiguous allocation (step 7).
- `kernel/object/` (new `iommu_dispatcher.*`, `bus_transaction_initiator_dispatcher.*`, `pinned_memory_token_dispatcher.*`), `kernel/lib/syscall/src/` (a new file for the calls) and `system/public/ours/`: the BTI model (step 5).
- `kernel/lib/userabi`, `system/service/init`, `system/lib/ours.resource`, `kernel/products/image/dev/BUILD.gn`: the IOMMU slot to a component (step 6).
- `kernel/platform/pc/interrupt.cpp`: IO-APIC lines (step 8).

## Test plan

- Step 1: `resource_test` shows the root MmIo creating, and a range over RAM, over a device the kernel drives, past the address space and overflowing each refused with AccessDenied, OutOfRange or InvArgs, and a frame the page allocator hands out refused. Both images boot.
- Step 3: the test prints the host bridge's vendor id 0x8086 read through the mapped ECAM window.
- Step 4: the kernel test shows a pinned range refused by decommit, resize and reclaim, a second pin counted, and everything allowed again once both are undone.
- Step 5: the kernel test shows pin, unpin and the quarantine of a PMT dropped without one, and the syscall-level refusals.
- Step 6: routing_checks gets the IOMMU resource, and a mutation that binds the wrong server makes it fail.
- Step 7: the test prints that the addresses of a pinned contiguous VMO are page aligned and contiguous for the whole size.
- Step 8: the test prints the arrival of a device interrupt on a PCI GSI.
