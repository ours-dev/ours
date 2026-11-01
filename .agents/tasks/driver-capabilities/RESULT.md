# driver-capabilities: what the kernel gives a userspace PCI driver, delivered

**Date**: 2026-10-31

## Delivered

The four requests of the userspace virtio-blk driver (`exp/fs-service`, zcode) are in the tree on `feat/driver-caps`. Below is the interface list for the driver's side, in the order of its acceptance flow: enumerate PCI, map the BARs, build rings in DMA memory, take the interrupt, read block 0. Where a call has a worked example, it is named: the checks in `system/service/init/test/routing_checks/routing_checks.cpp` call every one of these end to end from a component, as a driver would.

### 0. What the component is handed

A component touches only what it is routed. The driver's manifest uses these protocols, and the root (`kernel/products/image/dev/BUILD.gn`, `root.cm`) has to offer them to it, as it offers `ours.resource.Irq` and `ours.resource.Ioport` to pc-ps2 and all of them to `routing_checks`:

| Protocol | Hands out | For |
|---|---|---|
| `ours.resource.Mmio` | the root of the physical address space | R1, R2: the ECAM window, the BARs |
| `ours.resource.Iommu` | the System IOMMU resource | R3: `iommu_create` |
| `ours.resource.Irq` | the root of the interrupt lines | R4: `interrupt_map`, `interrupt_create` |
| `ours.resource.Ioport` | the root of the I/O ports | only for a legacy virtio, whose BAR 0 is I/O ports |
| `ours.acpi.Route` | which GSI an ISA IRQ arrives on, and where a PCI segment's ECAM window is, from the firmware's tables | R4 for a legacy (ISA) device only; a PCI function's GSI is not answered yet (see "Where the GSI comes from") |

In code: `ours_resource::get(ours_resource::MMIO_PROTOCOL)` and the like (`//system/lib/ours.resource:client`; `system/driver/pc-ps2/pc_ps2.cpp` is a complete example). Each is a duplicate of the root, so the grant is all or nothing: a driver that holds `Mmio` may map any device, not only its own (`docs/repairs/261030/kernel-object.md`, item 2).

### 1. PCI configuration space (R1) and the BARs (R2)

Both are physical memory behind an `Mmio` resource, mapped uncached.

```text
vmo_create_physical(mmio, paddr, size, &vmo)      paddr and size page aligned; size not 0
vma_map(vma, options, offset, vmo, 0, size, &va)  always populated, always uncached
```

- The kernel refuses (`AccessDenied`) a window over RAM or over a device it drives itself (the local APIC, the IO-APICs, the HPET). Anything else the root covers is granted.
- The VMO cannot be resized, cloned or read with `vmo_read`; read and write it through the mapping, with accesses of the device's register width.
- Configuration space is ECAM: function `bus:dev.fn`, register `off`, is at `ecam_base + (bus << 20 | dev << 15 | fn << 12 | off)`. **The base is not discovered**: it is 0xE0000000 on QEMU's q35 under the edk2 firmware the images boot with (SeaBIOS uses 0xB0000000), and the driver can ask the route service for it, `ours_acpi::ecam_base(0)` (`system/lib/ours.acpi:client`), which reads ACPI's MCFG, or hard-code it as `routing_checks` does. `routing_checks` (`check_physical_vmo`) maps the host bridge's page and reads vendor 0x8086.
- BARs are assigned by the firmware: read them from configuration space (sizing a BAR is the driver's job), do not copy the addresses below.

### 2. DMA memory (R3)

A device's right to start DMA is a Bus Transaction Initiator (BTI); the memory it is given is a contiguous VMO pinned through it.

```text
iommu_create(iommu_resource, IommuType::Stub, nullptr, 0, &iommu)   desc must be empty for the stub
bti_create(iommu, 0, bti_id, &bti)                                  one per device; bti_id is the device's, any u64 for the stub
vmo_create_contiguous(bti, size, alignment_log2, &vmo)              memory a device is given one address of
vma_map(vma, ..., vmo, ...)                                         for the driver's own reads and writes
bti_pin(bti, PermRead | PermWrite [| Contiguous | Compress], vmo, offset, size, addrs, count, &pmt)
pmt_unpin(pmt)                                                      when the device is done with it
bti_release_quarantine(bti)                                         after a PMT was dropped without unpin
```

- `bti_pin` writes the device-visible addresses into `addrs`: one per page, one per contiguous run with `Compress`, or one for all of it with `Contiguous`. **With the stub IOMMU these are physical addresses**: the IOMMU translates nothing and isolates nothing, so a device can reach all of memory (`docs/repairs/261030/kernel-object.md`, item 3).
- A pin keeps the pages from being reclaimed and the VMO from being cloned (decommit and resize do not exist yet and will have to refuse a pinned range: `docs/repairs/261030/mm-pin.md`, item 2). A PMT that loses its last handle without `pmt_unpin` is quarantined: the pages stay pinned, because the device may still be writing to them, until `bti_release_quarantine`. A driver that dies with memory pinned therefore leaks it (item 4 of the same file).
- Worked examples: `check_iommu_and_bti`, `check_pin_addresses`, `check_quarantine`, `check_contiguous_vmo` in `routing_checks`.

### 3. The device's interrupt (R4)

This is the part that changed shape on the way (see Deviations). The rules first:

1. **A line is named by the kernel's number for it (a "virq"), never by a GSI.** `interrupt_create`'s `line` is a virq. A program obtains one and does not compute it, does not assume it is the same on another machine, another boot or another kernel, and does not assume it equals any number the hardware or the firmware uses: it is always above every GSI, so a GSI taken for a virq finds no line (`NoResource`).
2. **Only one component should know a number: the one that reads the firmware's tables.** It calls `interrupt_map` with the interrupt root and hands the device driver the interrupt object over a channel, not a number. In P1 zcode's driver is both, so it makes the two calls itself, and holds the whole interrupt root until a call that narrows a resource exists (P2).
3. **The device's cause must be cleared before `interrupt_ack`.** A PCI INTx is level-triggered: the kernel masks the line from the moment the interrupt is delivered, and `interrupt_ack` unmasks it. If the device still asserts its pin then, the same interrupt arrives again at once. For a virtio-pci device, reading the ISR status register clears it; do that, then ack.

The calls, in order:

```text
(config space)  interrupt pin = byte at offset 0x3D, 1 = A ... 4 = D;   slot = the device number
gsi = 20 + ((slot + pin) & 3)             for a function on bus 0, slot 0 to 24, pin A = 0   (see "Where the GSI comes from")
interrupt_map(irq_root, gsi, &line)                                                 the platform's number for the pin: a GSI
interrupt_create(irq_root, line, InterruptOptions::ModeLevelLow, &interrupt)       PCI INTx is level, active low
port_create(...) + interrupt_bind(interrupt, port, key, Bind) + port_wait(...)     or interrupt_wait(interrupt, &timestamp)
  ... read the device's ISR (clears the cause) ...
interrupt_ack(interrupt)
```

- `interrupt_map(resource, source, out)`: `source` is the platform's own number for the pin, which is a GSI on this (ACPI) platform. There is no `kind` and no ISA number (interrupt-system D11): a legacy device's ISA IRQ is turned into a GSI by whoever reads the firmware's tables, the ACPI route service `ours.acpi.Route` (`ours_acpi::isa_irq(n)`, which gives the GSI with its trigger and polarity; pc-ps2 is the example), and the GSI is what the kernel is given. `NoResource` for a source with no line; `InvArgs` for `0xFFFFFFFF`; `BadHandle`, `BadType`, or `AccessDenied` unless the resource is the interrupt root; and `AccessDenied` again, with the root accepted, for a pin whose line the kernel holds for its own use (the timer's), which no program can take. Nothing is taken until `interrupt_create`.
- `interrupt_create` answers `AlreadyBound` if another program's interrupt holds the line, and `AccessDenied` for a line the kernel holds for its own use (the timer's), which no program can take; `interrupt_map` answers `AccessDenied` for such a pin too, after it has accepted the root (changed 2026-11-01, `.agents/tasks/irq-multi-arch/`, step 5). The line is unmasked once the object has it, and masked again when the object is destroyed or its last handle closed: a driver that dies hands the line back, masked.
- Bound to a port, an interrupt sends one packet, and interrupts before the ack fold into the next packet, which the ack sends.
- The interrupt is delivered to the boot CPU, on a fixed vector. There is no MSI or MSI-X yet (P3).

**Where the GSI comes from.** The device's Interrupt Line register is not it: on the test machine the virtio-blk reads 11 there, a legacy PIC number the firmware wrote. The wiring is the chipset's, and the firmware describes it to the operating system in ACPI's `_PRT`, which nothing in the tree evaluates yet. For P1 the driver uses QEMU q35's rule (the user's decision, 2026-10-31):

- A function on the root bus at slot S, pin P (A is 0): GSI = 20 + ((S + P) & 3) for slots 0 to 24; GSI = 16 + P for slots 25 to 29 and 31.
- It comes from QEMU 8.2.2's source (`hw/isa/lpc_ich9.c`, `ich9_cc_init()` and `ich9_pirq_to_gsi()`; `hw/i386/acpi-build.c`, `build_q35_routing_table()`), and it was checked against the `_PRT` the firmware gave a booted guest, decoded from a memory dump: slot 3, pins A to D, GSI 23, 20, 21, 22; slot 1, 21, 22, 23, 20; slots 4 and 24, 20, 21, 22, 23; slots 25, 29 and 31, 16, 17, 18, 19.
- The first function QEMU adds, a virtio-blk at slot 3 pin A, is therefore on **GSI 23**.
- **It was not observed that an interrupt arrives there**: no device in the tree has been made to raise one. If the driver's first interrupt never comes, suspect this rule first. It holds for QEMU's q35 root bus only: devices behind a bridge, and real hardware, need the `_PRT` (`docs/repairs/261031/x86-apic.md`, item 2).

### 4. The test machine

```bash
DISK=/path/to/raw.img python3 scripts/qemu_boot.py
```

`DISK=` attaches the raw file as a virtio-blk disk (it is also passed through by `scripts/kshell_run.py` and `.agents/skills/ours-verify/scripts/boot_check.py`). The file is opened `snapshot=on`: writes go to a private overlay and are lost, so the two images `boot_check` starts side by side can share it. The device is QEMU's transitional virtio-blk-pci: 1af4:1001, subsystem 1af4:0002, at bus 0, device 3, function 0, with a legacy I/O BAR 0, a memory BAR 1 and a 64-bit prefetchable memory BAR 4 (the modern capabilities). The firmware assigns the BAR addresses; read them.

### Commits

- R2, R1: `de530be` the MmIo resource kind, `07d770e` `ours.resource.Mmio`, `1d9a7b4` `vmo_create_physical`, `128771c` uncached user mappings of a physical VMO.
- R3: `3d45ee4` pinning a range, `4dce652` the IOMMU, BTI and PMT objects, `da288c3` the calls, `9db96e3` a fix to `pmt_unpin`, `06200f0` `ours.resource.Iommu` to a component, `589f7f2` `vmo_create_contiguous`.
- R4 (the plan: `0fe1a8b`, decisions: `86311d2`): `2f6c129` `DISK=`, `1a0e04b` every IO-APIC pin is a line, `b55a682` a linear domain and lines named by virq, `4a7a90d` `interrupt_map`, `d2624a1` its review, `b5ed141` `RemapIrq` retired, `bba33f7` the checks' PCI pin. After the hand-over: the ACPI tables to user space and the route service (`89ce1ea`, `0ff14b3`, `6318f27`, `7daba6b`, `6e53d9c`), pc-ps2 on the service, and `interrupt_map` without `kind` (`git log` after `6e53d9c`).

## Verification

Run in a cloud container with substituted tools (QEMU 8.2.2 and Ubuntu's OVMF 2024.02, not the project's QEMU 11.1.1; GRUB 2.12-1ubuntu7; gn `1000 (1a7c151)`; LLVM 23.1.0-rc3, ninja 1.13.2 and Rust 1.96.0 as the project's; no KVM). The details are in the change records of `docs/changes/261031/` and in the earlier ones of `261030`.

- `boot_check.py` on the head: dbg PASS, 44 passed, 4 skipped, 0 failed; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required; host tests 151 passed, 0 failed at the time of the hand-over (154 and more since; see the later records). The same with `DISK=` set. `routing_checks` runs the calls above from a component on the rel image.
- Kernel tests, all ok: `resource.test` 72 checks, `physvmo.test` 6, `vmaphys.test` 8, `vmopin.test` 28, `bti.test` 79, `vmocontig.test` 16, `pinlines.test` 28, `irqdomain.test` 39, `interrupt.test` 32, `interruptline.test` 59, `irq.test` 72.
- Mutations: every new check of the interrupt work was shown to fail under a deliberate fault in the code it covers (identity virq map, interrupt path looked up by virq, root test off, resource not checked, ISA bound on 8 bits, `RemapIrq` honoured when not asked for, and others), and the files restored byte for byte.
- Independent reads: two rounds of read-only reviewers with a skeptic for each finding (`b55a682`: 11 findings, `4a7a90d`: 9, all low), whose findings are fixed or recorded.
- Not measured: **a device raising an interrupt that reaches a program** (no driver is in the tree; the user decided the virtio-blk driver is the first test); the modern (MMIO) virtio path; MSI and MSI-X; real hardware; QEMU 11.1.1 and its firmware, where the ECAM base, the device ids, the BAR layout and the GSI rule should all be read again.

## Deviations from the design

- R4 was first a single step, registering the IO-APIC pins past the ISA range (`DESIGN.md`, R4). It became the `interrupt-system` task (`.agents/tasks/interrupt-system/`) on the user's decision of 2026-10-31 that a user program never handles a GSI: lines are named by the kernel's number, there is a call that turns a firmware source into it, and `RemapIrq` is gone.
- The plan had the driver read the device's Interrupt Line register for the GSI. The register was found to hold a legacy PIC number (11), so the GSI comes from q35's wiring rule instead.
- Step I5 of `interrupt-system` (a device interrupt arriving, in the tree) is not done, by the user's decision of 2026-10-31: this hand-over replaces it, and the driver is the first test of a device interrupt.
- `RemapIrq` and `platform::remap_isa_irq()` are deleted rather than kept, as `interrupt_map` and `platform::isa_irq_line()` replace them.

## Found along the way

- A pin the firmware wired to NMI is overwritten by the platform's masked entry: `docs/repairs/261031/x86-apic.md`, item 1.
- The GSI of a PCI function comes from a rule that holds for QEMU's q35 only: `docs/repairs/261031/x86-apic.md`, item 2.
- The checks took GSI 23 as their PCI pin, which is the pin a virtio-blk is on: fixed in `bba33f7`.
- Review findings of the interrupt work (a clock event device keeping a pin in its virq slot, stale documentation, checks that let a wrong change through): fixed in `b55a682` and `d2624a1`.
- `clang-tidy` reports two naming warnings in `kernel/arch/x86/kernel/time/hpet.cpp` (`id_`, `regs_mmio_`), which are older than this work and were left.

## Not done

- A user-space ACPI component that evaluates `_PRT`, which replaces the q35 rule (`.agents/tasks/interrupt-system/PLAN.md`, P2, step 8).
- Narrowing a resource, so that a driver holds only its device's range and its interrupt (`docs/repairs/261030/kernel-object.md`, item 2; P2), and exclusive ranges (item 6).
- A real IOMMU: the stub translates nothing and isolates nothing (item 3). Reclaiming memory a dead driver left pinned (item 4), and a limit on pinned memory (item 5).
- A cap on a physical VMO and large page-table entries for it (item 1); pinned ranges refusing decommit and resize (`docs/repairs/261030/mm-pin.md`, item 2).
- MSI and MSI-X, dynamic vectors, a target CPU per interrupt (`interrupt-system`, P3 and P4).
- A driver that takes the ECAM base from the route service (`ours_acpi::ecam_base`) instead of a constant: the service answers it since `6e53d9c`, and no driver asks yet.
