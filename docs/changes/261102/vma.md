# vma changes

## Title: a writable mapping of a physical VMO is the handle's rights to allow, not the type's to refuse (2026-11-02)

**Files**
- `kernel/object/vma_dispatcher.cpp` — `VmaDispatcher::map()` no longer refuses `PermWrite` on every physical VMO; a physical mapping still gains `Discache` unconditionally.

**Symptom**
`init: virtio-blk exited with 1`, and with it `virtio-blk: cannot map the ECAM window: InvArgs`, `btreefs: no volume to serve`, and every disked check of `routing_checks` skipping silently. Every boot since 40a29b42, on both images, with or without a disk attached.

**Cause**
The clock-page commit (40a29b42) added a type-level rule to `map()`: a mapping with `PermWrite` of a `Type::Physical` VMO is `InvArgs`, because a writable user mapping of the HPET's running counter could stop the counter or rewrite a comparator. The intent was to protect one page; the check covered every physical VMO. But a driver's whole reason to hold a physical VMO is to read and write its device's registers: `virtio-blk` maps its ECAM window `PermRead | PermWrite`, and the check refused exactly that, before mm ever saw the request. The first user of `vmo_create_physical` that needed write lost the syscall the same day it shipped, and the branch's disked verification went hollow — the mount checks skip when no block device answers, so `routing_checks: passed` kept printing while the disk stack was dead.

The protection it wanted was already there, one layer up and one mechanism more precise: `CLOCK_PAGE_RIGHTS` hands the clock page out without `Rights::Write`, and `vma_map` refuses `PermWrite` when the handle pair does not grant Write (`AccessDenied`, syscall `vma.cpp`). Rights attach to that one handle and cannot be widened by duplicate or child; the type-level refusal attached to every physical object in the system, including the ones that must be written. Protection at the granularity of the capability, not of the class.

**Change**

```cpp
// before
if (vmo.vmo()->type() == mm::VmObject::Type::Physical) {
    mmuf |= mm::MmuFlags::Discache;
    if (!!(options & VmOptions::PermWrite)) {
        return ktl::err(Status::InvArgs);
    }
}

// after
if (vmo.vmo()->type() == mm::VmObject::Type::Physical) {
    mmuf |= mm::MmuFlags::Discache;
}
```

Rejected: keeping the refusal and carving out the clock page by address (`base == hpet_mmio_phys()`) — one physical VMO would be special-cased in generic mm code, and the next read-only register page would want its own carve-out; and marking the object "no-write-map" at creation — a new VMO flag for a property the handle rights already express.

**Reference**
Zircon splits the same way: what a mapping may do is decided by the handle's rights at `zx_vmar_map` (`kernel/object/vm_address_region.cpp`, `VmAddressRegion::Map()`), and MMIO writability is a property of who holds the resource, not of the physical class (`ZX_RIGHT_WRITE`). The uncached discipline stays ours: every physical mapping is `Discache` regardless of rights.

**Verification**
Disked `boot_check.py`: dbg PASS (45), rel PASS (43, 7/7 required, `routing_checks: passed`), host-tests PASS 159. The rel log shows the chain the fix restored: `virtio-blk: region at 0x580000`, `virtio-blk: serving ours.block.Block`, `btreefs: serving the volume at /disk`, and the GPT suite replacing the skipped plain checks. Bisect of the regression window: `b6901a7d` clean, `7ccd58ed` broken, pinning it to 40a29b42; the two commits between do not build (7ccd58ed fixed 40a29b42's leftovers), so the intermediate point was not bootable. A temporary diagnostic line in the driver's `map_mmio` split the two calls: `vmo_create_physical` succeeded, `vma_map` answered `InvArgs` — the refusal was mm's, not the resource's. clang-format clean.

**Deliberately not done**
- A test that pins "writable physical mapping works": virtio-blk's own disked checks are that test, and a synthetic one would need a second physical VMO in the image.
- Refusing `PermWrite|PermExecute` combos anew: W^X is checked before, unchanged.

**How it was found**
The merge's disked verification printed `btreefs: no volume to serve` while `routing_checks: passed` — a pass that the mount checks' skip made hollow. Bisecting the branch (clean at 010b5f6a and b6901a7d, broken at 7ccd58ed) framed 40a29b42; a one-line diagnostic in the driver split the failing call; the InvArgs paths of `VmaDispatcher::map()` and its syscall wrapper were read until only one survived the arguments the driver passes. The magic-scan corruption theory — the vDSO scan binding a `cmp` immediate instead of the symbol — was checked against the built image and died: the magic appears exactly once, at the symbol.

**The user's prediction**
None. A regression found while verifying the merge.

**Three questions you should be able to answer**
- The clock page handle carries no `Write`, yet the kernel still maps its pages `Discache` — which of the two mechanisms stops a holder from *caching* a stale counter, and which stops it from *writing* the counter?
- Why is a type-level refusal in `map()` worse than a rights check at the syscall even for the page it was written for — what would `fsio`'s `clone` of a narrowed handle have to trust?
- The skip made the diskless boot print the disk suite's pass. What would have caught it on the day 40a29b42 landed, and why did the required-checks list not?
