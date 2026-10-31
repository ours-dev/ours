# driver-capabilities: the kernel side of a userspace PCI driver, run through first

**Date**: 2026-10-30
**Status**: Proposed

## Request

"先跑通吧": get the path working end to end before widening it. The requester is the userspace virtio-blk driver on `exp/fs-service` (`.agents/tasks/storage-layer/KERNEL-REQUESTS.md`). The scope ends where that driver's acceptance flow can run: enumerate PCI through ECAM, map the BARs, build rings in DMA memory, bind a PCI interrupt, read block 0. The narrowing syscall, uncached mappings, ACPI and MSI-X are out of scope and go to `docs/repairs/`.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Physical VMO entry point | A: flag on `vmo_create`; B: `vmo_create_physical` | B | None | B, 2026-10-30 |
| Gating resources | MmIo and a System slot; or a new Dma kind | MmIo and a System slot | None | MmIo and a System slot, 2026-10-30 |
| R1 transport | A: open 0xCF9; C: `pci_cfg_pio_rw`; D: ECAM through MmIo | D | None | D, 2026-10-30 |
| R4 route | Shortcut; standard (userspace ACPI); MSI-X | Shortcut | None | Shortcut, 2026-10-30 |
| MmIo denial pattern | A deny list checked even for the root; or only a region allocator | Deny list | None | Deny list, 2026-10-30. Zircon uses the same deny list (`RootResourceFilter`), so this is no departure; the question as first put said it was |
| Exclusive MmIo ranges | Track overlap and an exclusive flag with a region allocator; defer | Defer | None | Open: not needed for the first run |
| DMA address and gate | Dma resource and info topic; BTI object | Resource and info topic | None | Open: a departure from Zircon, to confirm |

## Steps

Each step is one commit and is introduced to the user before it starts.

1. The `MmIo` resource kind. `ResourceDispatcher::create` accepts it, the root covers the physical address space, and `validate_ranged_resource()` refuses a range that touches RAM (a copy of the memory manager's map, `kernel_ram()`) or MMIO the kernel drives (`kernel_mmio()`, beside `kernel_ioports()`). Verified by `resource_test` and `ours-verify`.
2. `ours.resource.Mmio` as a built-in served by the root, so a component can use it from its manifest: the kernel's userboot mints the MmIo root, the user-mode userboot passes it on, the root serves it with a ResourceServer, and routing_checks gets it from root.cm. Verified by a boot where routing_checks receives it and object_get_info says it is the MmIo root.
3. `vmo_create_physical(resource, paddr, size, out)` and mapping it. Verified by a test that maps QEMU's ECAM window (0xE0000000 under edk2) and reads the host bridge's vendor id.
4. The Dma resource, `VmoOptions::Dma` on `vmo_create` (which gains a `resource` argument), `VmCowPages::Flags::Contiguous`, and `InfoTopic::VmoPhys`. Verified by a test that checks contiguity, pinning and the refused operations.
5. IO-APIC pins past the ISA range registered as lines. Verified by `interrupt_create` on a PCI GSI and an interrupt arriving from a virtio device.
6. Hand the interface list back to the `exp/fs-service` side, and record the deferred items in `docs/repairs/`.

## Files

- `kernel/object/resource_dispatcher.cpp` and its header: the MmIo kind and its validation (step 1).
- `kernel/platform/` (pc, arm, riscv) and `kernel/platform/include/ours/platform/`: `kernel_mmio()` (step 1).
- `kernel/commands/resource_test.cpp`: the MmIo cases (step 1).
- `system/service/init/builtins.json` and `resource_server.*`: the built-in (step 2).
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/vmo.cpp`: `vmo_create_physical`, `vmo_create` (steps 3 and 4).
- `kernel/object/vmo_dispatcher.*`, `kernel/mm/vm_cow_pages.*`, `system/public/ours/object_info.hpp`: DMA memory (step 4).
- `kernel/platform/pc/interrupt.cpp`: IO-APIC lines (step 5).

## Test plan

- Step 1: `resource_test` shows the root MmIo creating, and a range over RAM, over a device the kernel drives, past the address space and overflowing each refused with AccessDenied, OutOfRange or InvArgs, and a frame the page allocator hands out refused. Both images boot.
- Step 3: the test prints the host bridge's vendor id 0x8086 read through the mapped ECAM window.
- Step 4: the test prints that the physical base is page aligned, that the whole size is contiguous, and that resize, clone and decommit answer NotSupported.
- Step 5: the test prints the arrival of a device interrupt on a PCI GSI.
