# pci-hotplug: delivered — the machine gains devices and the guest claims them

**Date**: 2026-11-06

## Delivered
- `feat(pci): the config-space walk and BAR sizing, over a fake ECAM` —
  system/lib/pci: the Config accessor, walk_bus0, the BAR
  sizing/assignment/enable, and later bridge.hpp's window programming.
- `feat(nvme): the resident driver, its Control rescan, and the device
  core` — system/lib/nvme (the register file, queue pairs, bring-up,
  probe), ours.nvme.Control.Rescan (codec + client + fake-provider
  suites), and the resident driver shape: NotFound is an answer, the
  block protocol appears with the disk.
- `feat(virtio): the resident driver, its Control rescan, and the device
  core` — the twin: Ports as the access seam, the legacy ring, the park
  the interrupt wait hangs on, the port-space IoArena; the GSI computed
  from the chipset's wiring instead of the boot disk's constant.
- `feat(hotplug): the shell's rescan, the QMP pipe, and the bridge
  windows` — pci-rescan, the root.cm offers, the QMP socket and
  scripts/qemu_device.py, the multi-bus walk, the bridge windows and the
  INTx swizzle, and the sizing fix the first hot-add caught.

## Verification
- Host suites: pci 10, nvme 14, virtio 15, and the four Control suites
  (codec + client, nvme + virtio) 6 each — all green.
- Coverage (cov_host, lines, the round's gate ≥95%): pci 100%,
  nvme 97.8%, virtio 96.7%, nvme_control 100%, virtio_control 100% (the
  drivers' own nvme.cpp / virtio_blk.cpp are target-only components and
  sit outside the host gate, as block-server's does).
- boot_check: dbg 46 passed / rel 44 passed, 7/7 required, on three
  machine shapes — no disk, virtio disk, nvme disk.
- The demo (the round's answer to "支持热拔插吗"): a machine booted with
  two pcie root ports and no disks takes an NVMe disk behind rp0 and a
  virtio disk behind rp1 through QMP device_add while it runs;
  `pci-rescan nvme` claims and publishes block/000, `pci-rescan virtio`
  claims and publishes block/001 — both leaves in the device tree, the
  registry's grant numbering naming them.

## Deviations from the design
The plan's five steps held; the differences are inside them:

- system/lib/nvme and system/lib/virtio were not in the plan's file
  list -- the 95% gate forced the device cores out of the target-only
  drivers, and the Sink/Ports seams (the fake device acts on stores) are
  the shape that made them testable on the host.
- The bridge windows, the INTx swizzle and the multi-bus walk were not
  planned at all: the first hot-add claim sat waiting on CSTS with the
  enable write going nowhere, and the round followed the data.

## Found along the way
- `walk_bus0` walked 32 function pages -- devices 0-3 -- not the 256 a
  bus carries; the rel machine's controller sat past the boundary and
  the driver answered NotFound on a machine that had a disk.
- A 64-bit BAR's size is the lowest set bit of its whole 64-bit mask;
  the complement at either half's width alone reads a hot-added
  device's all-ones high half as four gibibytes of decode, which the
  arena refused with NoResource on the first hot-add.
- The interrupt pin register is 0x3D; 0x3C is the line register, which
  the platform answers 0x0B on -- a debug line read the wrong offset and
  the record keeps the lesson.
- QEMU drops a virtio device's legacy I/O BAR on a PCIe link unless
  disable-legacy=off, and pcie.0 takes no hotplug: the QMP pipe spells
  both.

## Not done
- The removal direction: device_del asks the guest's ACPI to eject, and
  the guest has no AML interpreter — the slot stays occupied. The
  software un-publish (the round before this) is the half that exists.
- One claim per driver per boot; a second claim is the next round's
  work. The arena's placement ([ecam_base − 64 MiB, ecam_base)) is a
  documented assumption on the machines this runs on.
