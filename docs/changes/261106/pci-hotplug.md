# pci-hotplug changes

## The resident drivers, their Control rescans, and the hot-add claim path (2026-11-06)

**Files**

- `system/lib/pci` — the config-space accessor over the ECAM window,
  `walk_bus0`'s in-use-function visit, the BAR sizing/assignment/enable,
  and `bridge.hpp`'s bridge-window programming.
- `system/lib/nvme`, `system/lib/virtio` — the device cores: register
  layout, queue pairs and the bring-up sequence (nvme), the Ports
  interface and the legacy ring (virtio), each with the probe and the
  arena. The host suites run a fake controller/device whose stores act.
- `system/lib/protocols` — `ours.nvme.Control` and `ours.virtio.Control`,
  the Rescan codec and typed clients, cross-refusing each other's
  messages.
- `system/driver/nvme`, `system/driver/virtio-blk` — resident: the boot's
  first rescan answers NotFound and the driver stays up serving its
  control; a rescan claims a newly appeared function, opens the root
  port's windows, brings the controller up and publishes the disk.
  `ours.block.Block` enters the outgoing only once a disk is claimed, so
  an Open before that reads as the refusal it is.
- `system/app/pci-rescan` — the shell's command: sends Control.Rescan,
  prints the answer.
- `kernel/products/image/dev/BUILD.gn` — the two Control offers to the
  shell; `scripts/qemu_boot.py`/`scripts/qemu_device.py` — the QMP
  socket and the host-side device_add.

**Symptom**

A machine gained a device and nothing in the guest noticed. The drivers
scanned the bus once at boot and exited when they found nothing; a device
that arrived later arrived unowned, with unassigned BARs, behind bridge
windows firmware had never opened.

**Cause**

PCI enumeration was modeled as boot work. It is not: firmware assigns
BARs at boot only -- a hot-added function arrives with an unassigned BAR
(base reads 0 over the type flag), and firmware assigns bridge windows
at boot only -- a hot-added function behind a root port arrives behind
closed windows, so even a correctly assigned BAR is unreachable: the
bridge forwards memory and I/O cycles for its secondary bus only over
the range its windows name. And the enumeration itself was modeled as
bus-0-only, which stops being true the moment a root port exists. Three
separate model errors, each sufficient alone to make hot-plug dead on
arrival.

**Change**

The drivers are resident: no device is an answer (NotFound), not an
exit. `ours.<family>.Control.Rescan` is the two-way trigger; the shell's
`pci-rescan nvme|virtio` sends it. A rescan walks every bus the ECAM
window names (one Config per bus slice), skips the functions it already
runs, sizes and assigns an unassigned BAR0 from an arena (memory: below
the ECAM window, inside the chipset's routable PCI hole; ports: the
port space's tail), opens the root port's windows over the arena, arms
the bridge, and swizzles the claim's INTx through the port from
config-space facts -- the bridge's slot and the function's interrupt pin
register (0x3D; 0x3C is the line register). Before:

```c++
// nvme.cpp, the scan-once shape: no device, no driver
if (device_offset == 0) {
    auto _ = ort::debug_line("nvme: no NVMe controller on bus 0");
    return 0;
}
```

After:

```c++
// nvme.cpp: NotFound is an answer, not an exit
if (first != Status::Ok) {
    auto _ = ort::debug_line({"nvme: no NVMe controller on bus 0 (",
                              ours::to_string(first), "); resident, waiting for a rescan"});
}
```

**Reference**

Linux: `pci_bus_assign_resources()` and `pci_setup_bridge()` -- the
window programming this round's `program_bridge_memory()` and
`program_bridge_io()` hand-roll; `pci_assign_unassigned_bus_resources()`
for the arena; Linux's `pci_scan_child_bus()` for the multi-bus walk.
Zircon: none -- Zircon's userspace PCI lives in `platform/pc` with
ACPI's _PRT for the interrupt routing this round computes from config
space, because the guest has no AML interpreter to read _PRT with.

**Verification**

- Host suites: pci 10 (walk, sizing, assignment, the bridge windows),
  nvme 14, virtio 15, nvme_control 6, nvme_control_client 6,
  virtio_control 6, virtio_control_client 6 -- all green.
- Coverage (cov_host, lines): pci 100/100 (bar.hpp, bridge.hpp,
  pci.hpp each 100%), nvme 97.8%, virtio 96.7%, the Control codecs 100%.
- boot_check: dbg 46 passed, rel 44 passed 7/7 required -- no disk,
  virtio disk (claimed, published, GPT block reads through), nvme disk.
- The demo, on target: a machine booted with two root ports and no
  disks; QMP device_add of an NVMe behind rp0 and a virtio-blk behind
  rp1 (disable-legacy=off -- QEMU drops the legacy I/O BAR on a PCIe
  link by default); `pci-rescan nvme` claims and publishes block/000,
  `pci-rescan virtio` claims and publishes block/001 -- the registry's
  grant numbering names the second leaf.

**Deliberately not done**

- `device_del`'s removal direction: QEMU's device_del asks the guest's
  ACPI to eject the function, and the guest has no AML interpreter to
  answer with -- the slot stays occupied (the demo hit exactly this, the
  stale id never freed). The removal direction is the un-publish
  software layer (landed the round before) plus a guest ACPI story, not
  this round's.
- Multi-claim: one claimed function per driver per boot (the storages
  are LazyInit); a second claim is the next round's work.
- The arena's placement is a documented assumption: [ecam_base - 64 MiB,
  ecam_base), inside the PCI hole of the machines this runs on; a
  machine whose RAM reaches there hands the arena a hole, which is a
  repair item, not a silent overlap.

**How it was found**

The walk boundary via boot_check's rel FAIL (the driver NotFound where
the machine had a disk); the sizing trap via the arena's NoResource on
the first hot-add; the bridge windows via bring-up waiting 30 s on CSTS
with the enable write going nowhere; the pin register via a debug line
that read 0x0B at 0x3C -- the line register -- where the pin was
expected.

**The user's prediction**

None.

**Three questions you should be able to answer**

- Why does a hot-added function need its BARs assigned by the guest, and
  what does the sizing write (all ones, read back) measure?
- What does a bridge's memory window mean, and what happens to a CPU
  access to a BAR behind a bridge whose window is closed?
- Why is the interrupt line behind a root port not the same GSI formula
  as on bus 0, and what config-space facts does the swizzle read?
