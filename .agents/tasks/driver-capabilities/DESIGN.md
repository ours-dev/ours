# driver-capabilities: what the kernel gives a userspace PCI driver

**Date**: 2026-10-30
**Status**: Proposed

## Problem

The storage layer's first component is a userspace virtio-blk driver (`exp/fs-service`, `.agents/tasks/storage-layer/KERNEL-REQUESTS.md`, requests R1 to R4). QEMU's x86 virtio-blk is a virtio-pci device, so the driver has to read PCI configuration space, map and touch the device's MMIO BARs, give the device memory it can address by physical address, and take the device's interrupt. None of these has a userspace path today: pc-ps2 gets by with I/O ports and one ISA line.

An audit of the kernel against the four requests found:

- R1: `ioports_request` could open 0xCF8 to 0xCFF, but `kernel_ioports()` lists 0xCF9 (the chipset reset register, written by `platform_reboot()`) inside that range, so the request is refused. Opening it would let any Ioport holder reset the machine.
- R2: `RsrcKind::MmIo` is a reserved number whose `ResourceDispatcher::create` answers NotSupported. `VmObjectPhysical`, `VmMapping::map_physical` and `VmoDispatcher::create_physical` exist, but nothing calls `create_physical`, and page tables carry no cache attribute.
- R3: `VmCowPages::Flags::Pinned` and the buddy allocator's contiguous blocks exist, but `vmo_create(size, options, out)` has no resource argument, `VmoOptions` has only `Resizable`, and no object_get_info topic returns a physical address.
- R4: `interrupt_create` already takes a line past the ISA range as it is and has `ModeLevelLow`, but `platform_init_apic` registers only the 16 ISA lines, so a PCI GSI answers NoResource.

## Goals and non-goals

Goals:

- The kernel provides basic capabilities, safety guarantees and permission checks, and knows nothing about any device: no PCI semantics, no virtio. Everything device-shaped lives in the driver.
- The smallest interface that lets the acceptance flow of KERNEL-REQUESTS.md run: enumerate PCI, map the BARs, build rings in DMA memory, take the interrupt, read block 0.
- Reuse the existing resource model: a capability is a resource handle that a component is routed from root.cm.

Non-goals:

- No IOMMU, no scatter-gather object (a Bus Transaction Initiator), no MSI or MSI-X.
- No ACPI or AML in the kernel, and no ACPI component yet (see Alternatives).
- No uncached mapping attribute, no resource narrowing syscall: both are recorded as repairs and wait.
- No performance work.

## Design

Each request, the Zircon counterpart (`~/Workspaces/projects/fuchsia/zircon`), and where ours differs.

### R2: `MmIo` resource and `vmo_create_physical`

- `RsrcKind::MmIo` becomes a ranged kind like Irq and IoPort: base 0 and size 0 is the root, which covers the physical address space (`1 << phys_addr_bits`).
- `validate_ranged_resource()` refuses, for every MmIo resource including the root, a range that touches RAM or MMIO the kernel drives. AccessDenied, as for ports. Both lists are kept by the platform seam (`kernel/platform/include/ours/platform/mmio.hpp`) in ordinary static storage:
  - the devices (LAPIC, each IO-APIC, HPET) are recorded by the PC platform's init code through `reserve_kernel_mmio()` and listed by `kernel_mmio()`, beside `kernel_ioports()`;
  - the RAM is a copy of the memory manager's map of usable memory, taken by an init hook and grown to whole pages, listed by `kernel_ram()`. It is a copy because the map itself (`mm::EarlyMem`) lives in the init sections, which `reclaim_init_area()` gives to the page allocator before userboot. A first version read the map directly and an independent review found that it would silently grant all RAM once those frames were reused; a second attempt through `boot::handoff().mem.ranges` faulted at run time, because the spans inside the handoff are physical addresses that only the early identity mapping can read.
  - If the copy was not made, or the device table overflowed, every address counts as owned: a refused request is the safe error.
- New syscall `vmo_create_physical(resource, paddr, size, out)` calls the existing `VmoDispatcher::create_physical` after `validate_ranged_resource(resource, MmIo, paddr, size)`. The VMO is not resizable, not clonable and not readable through `vmo_read` (NotSupported already). `vma_map` maps it through the existing `map_physical`.
- Zircon: `sys_vmo_create_physical` (`kernel/lib/syscalls/driver.cc:107`) validates with `validate_resource_mmio` and relies on `PhysicalAspace` having subtracted RAM from the MMIO allocator (`x86_resource_init_hook`, `kernel/platform/pc/memory.cc`). Ours keeps the deny-list-at-use pattern that `IoPort` already uses instead of an allocator, so the root resource stays a plain range.

### R1: PCI configuration space through ECAM

- No port syscall, and 0xCF9 stays in `kernel_ioports()`. The driver maps the ECAM window with `vmo_create_physical` and reads `ecam_base + (bus << 20 | dev << 15 | func << 12 | offset)` as memory.
- The ECAM base is hardcoded in the driver for QEMU's q35 for now (to be confirmed in the machine's memory map). Reading it from the ACPI MCFG table waits for an ACPI component.
- Zircon: the legacy port path is `zx_pci_cfg_pio_rw`, now deprecated in favour of the PCI protocol; its PCI bus driver is userspace and maps ECAM through an MMIO resource.

### R3: DMA memory

- New resource: a System slot for DMA (`ours.resource.Dma`), a yes or no privilege with no range.
- `vmo_create(resource, size, options, out)` gains a leading `resource` argument, ignored unless `VmoOptions::Dma` is set (precedent: `interrupt_create` with `Virtual`).
- `VmoOptions::Dma` means committed at creation, physically contiguous, pinned (`VmCowPages::Flags::Pinned`), and fixed: no resize, no clone, no decommit. It is built by a new `VmCowPages::Flags::Contiguous` that allocates one buddy block with `alloc_frames(order)`.
- The physical address comes back through a new `InfoTopic::VmoPhys`, which answers only for a Dma VMO.
- Zircon: `sys_vmo_create_contiguous` (`kernel/lib/syscalls/driver.cc:51`) is gated by a BTI handle, and addresses come from `zx_bti_pin`. Ours has no IOMMU and no BTI, so the gate is the Dma resource and the address is a VMO info topic. This is a departure from Zircon and is recorded as a decision.

### R4: the device's interrupt

- `platform_init_apic` registers every IO-APIC pin as a line, masked, with vector `PlatformIrqMin + GSI` as it does for ISA lines. `interrupt_create(resource, gsi, ModeLevelLow)` then works for a PCI line with no change to the syscall.
- Which GSI a device is wired to is the driver's business. For now the driver reads the device's Interrupt Line register and falls back to a q35 rule; both are to be checked against QEMU.
- Zircon: the x86 board driver evaluates the ACPI `_PRT` in userspace and hands the kernel a table (`dev_pin_to_global_irq`). Ours defers that.

### Resources

All three privileges reach a component the usual way: a built-in `ours.resource.MmIo` and `ours.resource.Dma` served by the root's `ResourceServer`, used in the driver's manifest, as `Irq` and `Ioport` are for pc-ps2. The server hands out a duplicate of the root's resource, so the grant is all or nothing until a narrowing syscall exists.

## Alternatives considered

- Open 0xCF9 so the driver can use ports 0xCF8 and 0xCFC: one line, but every Ioport holder could reset the machine.
- A kernel `pci_cfg_pio_rw` syscall (Zircon's legacy path): safe and small, but x86-only and a second route that ECAM makes unnecessary.
- Physical VMOs as a flag of `vmo_create` with a `paddr` argument: a flag carries no address, and `paddr` would be meaningful for one flag only. A separate `vmo_create_physical` keeps the "window onto hardware" object apart from memory with extra promises.
- A new `RsrcKind::Dma`: would need a range that means nothing for DMA. A System slot is the existing yes or no privilege.
- Probing for the interrupt line by unmasking every pin: unreliable for shared PCI lines and risks a storm.
- Reading `_PRT` now (ACPICA or the Rust `acpi` crate in a userspace component): the standard route and correct on hardware, but a large piece of work and a new third-party dependency that needs the user's approval.
- MSI-X: needs no wiring lookup, but needs a vector allocator in the kernel and the BAR mapping of R2 first.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Physical VMO entry point | A: flag on `vmo_create` with `paddr`; B: separate `vmo_create_physical` | B | None | B, 2026-10-30 |
| Gating resources | MmIo for physical, System slot for DMA; or a new Dma kind | MmIo and a System slot | None | MmIo and a System slot, 2026-10-30 |
| Narrowing syscall | Build now; defer | Defer | None | Wanted, but run-through first, 2026-10-30 |
| R1 transport | A: open 0xCF9; C: `pci_cfg_pio_rw`; D: ECAM through MmIo | D | None | D, 2026-10-30 |
| R4 route | Shortcut: open all pins, driver reads Interrupt Line; standard: userspace ACPI; MSI-X | Shortcut | None | Shortcut, 2026-10-30 |
| MmIo denial pattern | Deny list at use, as IoPort; region allocator, as Zircon | Deny list | None | Deny list, 2026-10-30: the user answered 可以 to the introduction of step 1, which asked about it |
| DMA address and gate | Dma resource and a VMO info topic; BTI object as Zircon | Resource and info topic | None | Open: a departure from Zircon, to confirm |

## Testing and verification

- `resource_test` (kshell): MmIo root creates; a range over RAM, over a device the kernel drives, past the address space, or overflowing is refused; a frame the page allocator hands out is refused; a page above everything is accepted; at every edge of every RAM run and device the root answers as a byte-by-byte model does; neither table lives in the init sections.
- A kshell or user test maps the ECAM window of QEMU's q35 and reads the host bridge's vendor and device id (8086:29c0); a second reads the virtio-blk function.
- A Dma VMO: physical address is page aligned and contiguous for the whole size, survives memory pressure (pinned), and rejects resize, clone and decommit.
- `interrupt_create` on a PCI GSI succeeds and an interrupt from the device reaches the port.
- `ours-verify` before each commit. Not verifiable here: real hardware, uncached MMIO behaviour, an IOMMU.

## Open questions

- Is the q35 ECAM base 0xB0000000 on this QEMU invocation? Check `info mtree`.
- Does the Interrupt Line register hold a GSI on q35 with the firmware these images boot with? Check `info pci`.
- Does the page allocator ever take ACPI reclaimable memory? If it does, the managed-memory check must treat it as managed.
- Does a Physical VMO need a cache attribute for the first run on QEMU? Believed not.
