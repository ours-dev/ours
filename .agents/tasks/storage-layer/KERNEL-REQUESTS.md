# storage-layer: kernel capability requests from the userspace driver work

**Date**: 2026-10-30
**Status**: Proposed (the requester's needs; the interfaces are the kernel side's to design)

## Context

The storage layer's first userspace component is a virtio-blk driver (a component like pc-ps2,
under `system/driver/`), followed by a GPT reader and the first on-disk filesystem — all userspace,
all consuming kernel capabilities through `/svc` resources. QEMU's x86 virtio-blk is a virtio-pci
device, so the driver must: enumerate PCI, read the device's config and BARs, map and touch MMIO,
give the device physically-contiguous memory by physical address, and take its completion
interrupt. None of these has a userspace path today: pc-ps2 gets by with I/O ports and one IRQ
line, and touches no DMA and no device memory.

These are the capability requests. The syscall and resource shapes are the kernel side's to
decide; each request states what the driver does with it, so the interface can stay minimal.

## Requests

### R1. PCI configuration-space access

To enumerate the bus and configure the device: find vendor `0x1af4` on QEMU, read the BARs and the
virtio capability list, and turn on bus-master and memory-decode bits in the command register.

On x86 this is the `0xcf8`/`0xcfc` port pair. An `ours.resource.Ioport` grant covering those ports
may be all it takes; if PCI deserves its own System resource slot (as Irq and Ioport have), the
driver's manifest will use whatever name is settled. The driver needs read and write of 16/32-bit
values through the existing `ours.resource.Ioport` protocol, or the new one.

### R2. Mapping device MMIO

Modern virtio-pci keeps its common config, ISR and notify registers in an MMIO BAR. The driver
needs to map a physical address range it learned from the BARs into its own address space, read and
write it, with the mapping gated by a resource — mapping arbitrary physical memory is a privilege
like vmex.

Today `vma_map` only takes VMO handles. The kernel side decides the shape: a physically-backed VMO
over a requested range, a `vma_map_physical` variant behind a resource check, or something else.
What the driver needs is: given a resource and a physical range, a readable/writable mapping in the
caller's space.

### R3. DMA memory: contiguous, pinned, physically addressable

The virtqueues (descriptor table, avail and used rings) and the block buffers live in memory the
device reads and writes by physical address. The driver needs, per allocation:

- physically **contiguous** memory (the split-virtqueue rings and legacy layout want it; 4 KiB
  granularity, sizes up to a few MiB);
- its **physical base address** (written into the device's queue configuration);
- pages committed and **pinned** for the process's lifetime — the device writes into them without
  the kernel knowing;
- cache-coherent (x86 is; stated so the kernel side knows no coherency work is asked).

A resource gate belongs on this, as on vmex: only a holder may create such memory or learn
physical addresses, or any process could aim DMA at the kernel. Fuchsia's counterpart is a Bus
Transaction Initiator grant; ours may want a System resource slot in that spirit, or a flag on
vmo_create plus a get_info topic, whichever the kernel side prefers.

### R4. The device's interrupt

Completions arrive on the device's interrupt. QEMU's virtio-pci offers legacy INTx and MSI-X;
legacy INTx is a GSI line, which is the smaller kernel ask and enough for a first driver.

The existing shape (an `ours.resource.Irq` grant + `interrupt_bind` to a port, as pc-ps2 uses)
fits, if the Irq resource can name a GSI/PCI interrupt pin and not only the ISA lines. What the
driver needs: one resource grant that binds the device's line to the driver's port, with the
acknowledge per the existing protocol.

## Not requested

- No MSI-X, no IOMMU, no DMA scatter-gather: QEMU's defaults need none of these, and a first
  driver with contiguous buffers does not either.
- No block-layer semantics from the kernel: the Block protocol, GPT parsing and the filesystem are
  userspace work, built on the four capabilities above.
- No devfs or device discovery: the first disk is routed statically in root.cm, userspace work.

## Acceptance, from the driver's side

The driver is this story: enumerate PCI (R1), map the device's BARs (R2), build the rings in DMA
memory (R3), bind the interrupt (R4), read block 0 — and log its first sector. Every capability
above is exercised by exactly that flow, in a component that holds only the resources root.cm
grants it.
