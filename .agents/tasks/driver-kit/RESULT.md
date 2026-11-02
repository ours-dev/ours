# driver-kit: the runtime helpers landed

**Date**: 2026-11-03

## Delivered

- `//system/lib/ddk` — `Resources` (injectable getter, `take`/`drop`, destructor unwind), `startup_handle`, `MappedMmio`, `Bti`, `DmaRegion`, `irq::take`. 27c1c241.
- The five-case host ladder suite on the fake kernel, wired into `//:default`. 27c1c241.
- virtio-blk on the kit, ECAM base from the route service (root.cm offers `ours.acpi.Route` to it), the `fail` lambda and `map_mmio` deleted — 114 lines out, and the file's own long-promised migration done. 69e0cddb.
- acpi-route's startup guards on `startup_handle`. e5248c03.
- Design and plan: b48a65ff. Change record: `docs/changes/261103/driver-kit.md`. The kit README.

## Verification

- Host: 161 passed, 0 failed (the kit's five among them).
- `boot_check.py`: dbg PASS (46), rel PASS (44, 7/7 required — pc-ps2's keyboard check among them, live).
- Disked: virtio-blk on the kit serves, btreefs mounts, `routing_checks: passed` with no not-run line.
- clang-format and clang-tidy clean on every changed file; documents linted.

## Deviations from the design

- The plan's step 3 (the hardware idioms as a separate commit) folded into step 2: the idioms were written into the one header together with the ladder, and splitting them out would have been a commit of choreography, not of change.
- The plan's pc-ps2 migration did not happen, deliberately: its resources are each taken, used and dropped immediately — there is no ladder — and `take_line`'s triple `interrupt_create` is the AlreadyBound kernel-behavior check, which `irq::take` would erase. Recorded in the change record.
- The plan's Loop/Outgoing wiring-tail helper was already a non-goal; it stayed one.

## Found along the way

- Two kit bugs flushed out by the first disked boot and by review: `MappedMmio`/`DmaRegion` never recorded the VMA they mapped into (destructors could not unmap; `DmaRegion`'s `vma_map` passed its own unset handle as the destination). Fixed in 69e0cddb — the argument for migrating a real driver in the same round as the library lands.
- The kernel-interrupts capability gap from the block-driver round (`docs/repairs/261103/kernel-interrupts.md`) stands; nothing here changed it.

## Not done

- The driver template and guide chapter — next round.
- virtio-blk's GSI from the route service — waits on a `PciIrq` method.
- pc-ps2 migration — deliberately, see above.
