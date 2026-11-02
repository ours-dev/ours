# driver-kit changes

## Title: the driver kit: the ladder, the guards, and the hardware idioms as objects (2026-11-03)

**Files**
- `system/lib/ddk/` — new library (landed as `driver-kit`, renamed to `ddk` within the round at the user's naming): `driver_kit::Resources` (the resource ladder with an injectable getter, `take`/`drop`, destructor unwind), `startup_handle` (the take-plus-line guard pair), `MappedMmio` (physical window mapped and unmapped), `Bti` + `DmaRegion` (the DMA chain and its unmap→unpin→close teardown), `irq::take` (map→create→close-the-root). Scaffolded with copier.
- `system/driver/virtio-blk/virtio_blk.cpp`, `BUILD.gn` — the four-resource ladder, both startup guards, the 28-line `fail` lambda, and the `map_mmio` helper deleted; the ECAM base now comes from `ours_acpi::ecam_base(0)` (`ours.acpi.Route` in the manifest's `use`, offered in root.cm) instead of the `0xE000'0000` chipset constant.
- `system/service/acpi-route/src/acpi_route.cpp` — its two startup guard pairs on `startup_handle`.
- `kernel/products/image/dev/BUILD.gn` — `ours.acpi.Route` offered to `virtio-blk`.

**Symptom**
None — an enabling capability. Every driver restated the same preamble: a resource get per protocol with a hand-rolled unwind that grows quadratic (virtio-blk's fourth failure closed three handles), startup-handle guards typed twice per program, and teardown orders — unmap before unpin, close the root after taking the line — kept in comments and `fail` lambdas, the parts a mistake turns into a kernel quarantine of pinned memory. virtio-blk still hardcoded the ECAM base its own comment promised the route service would name.

**Cause**
The idioms existed only as examples to copy, and the copy is where the orders drift. The kit's answer is the same one the block-server library made for the protocol: the object owns the order. The design's alignment rule — helpers consume handles their caller holds; only `Resources` acquires, through a getter a host test replaces — is what keeps the kit valid when the driver manager's `Claim` lands: `Claim` returns handles, and every helper keeps taking handles.

**Change**

```cpp
// virtio-blk's preamble, after
driver_kit::Resources resources;
auto const mmio = resources.take(driver_kit::Kind::Mmio);   // ... Ioport, Iommu, Irq
auto const ecam = ours_acpi::ecam_base(0);
auto ecam_window = driver_kit::MappedMmio::map(vma.unwrap(), mmio.unwrap(),
                                               ecam->address, usize{ecam->size});
...
auto interrupt = driver_kit::irq::take(irq.unwrap(), ours::ROOT_INTERRUPT_DOMAIN,
                                       GSI_DISK, ours::InterruptOptions::ModeLevelLow);
resources.drop(irq_resource.unwrap());   // the root goes back to the pool
```

The migration flushed two kit bugs before they could ship: `MappedMmio` and `DmaRegion` never recorded the VMA they mapped into — their destructors could not unmap, and `DmaRegion`'s own `vma_map` passed its unset `m_vma` as the destination (BadHandle on the first disked boot). Both now take the caller's VMA handle explicitly. pc-ps2 was examined and deliberately left: its two resources are each taken, used and dropped immediately — there is no ladder for `Resources` to own — and its `take_line`'s triple `interrupt_create` is the AlreadyBound kernel-behavior check, which `irq::take`'s single create would erase.

**Reference**
The ladder-as-object is Linux `devm_*`'s answer at user-space scale: resources owned by a scope, torn down in reverse acquisition when the scope leaves. The teardown orders are this kernel's own (pin discipline: `bti_release_quarantine` exists because someone got the order wrong). The route-service ask is Fuchsia's `acpi` ban on drivers reading firmware tables themselves.

**Verification**
Host: the kit's five-case ladder suite on the fake kernel (all-acquired, a mid-ladder failure leaving the prefix to the destructor, `drop` without disturbing the rest, the table running out, the startup guard's line and status), 161 host tests, 0 failed. Target: `boot_check.py` dbg PASS (46), rel PASS (44, 7/7 required — the keyboard check exercising pc-ps2's untouched interrupt path live); disked, virtio-blk on the kit serves, btreefs mounts, `routing_checks: passed`. clang-format and clang-tidy clean on every changed file.

**Deliberately not done**
- pc-ps2 migrated: recorded above — no ladder, and the ritual is the test.
- The copier driver template and the guide's driver chapter: the next round, once this API settles.
- virtio-blk's GSI from the route service: the route has no `PciIrq` method yet; the driver-manager design carries it.
- A helper for the Loop/Outgoing wiring tail: the storages belong to the driver (no heap), and a helper would move lines without removing any.

**How it was found**
None — a planned round. The two kit bugs were found by the migration's first disked boot (BadHandle from `DmaRegion`'s unset VMA) and by review (the unmap leak), which is the argument for migrating a real driver in the same round as the library lands.

**The user's prediction**
None. The user scoped the round (kit library first) and approved the plan.

**Three questions you should be able to answer**
- Why does `Resources::take` record the handle instead of returning it and forgetting — what does the fourth failure's error path look like with and without the ladder?
- The kit takes the caller's VMA handle in `map`/`allocate` instead of taking the roots itself. Which future change is that shape sized for, and what would break first if the helpers acquired on their own?
- pc-ps2 was left unmigrated. What in its structure makes the kit a poor fit — and which of its lines would `irq::take` have deleted that must not be deleted?
