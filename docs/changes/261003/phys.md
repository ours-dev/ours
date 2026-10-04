# phys changes

## Title: The boot image is reserved in whole pages, from a page-aligned base (2026-10-03)

**Files**
- `kernel/phys/include/ours/phys/boot_package_input.hpp` — `BootPackageInput::from()` refuses a base that is not page-aligned and a size whose page-rounded end would wrap. `reserve()` reserves `[base, align_up(base + size))`. `bytes()` is unchanged: exactly the image.

**Symptom**
None: step 1 of `.agents/tasks/boot-image-pages/PLAN.md`. The kernel is to hand the boot image's own frames to userboot in a VMO, which takes whole frames.

**Cause**
The objects:
- The boot image (the OBI container). Its base is page-aligned, because multiboot refuses anything else, and its size is not a page multiple: 1210624 bytes on rel.
- Its bootmem reservation, which was exact to the byte.
- `stamp_reserved_frames()`, which marks a frame Reserved and counts it under the first reservation that touches it, skipping a frame already stamped.

With an exact reservation, the frame holding the image's tail is half free as far as bootmem knows. Something else could be placed in its other half and stamped first. The frame would then count under that owner, and the image's adoption could give away a frame that still holds live data. A frame has to belong to one owner, so the image's reservation takes its last frame whole.

The slack is not zeroed here, though Zircon zeroes it in phys. The RSDP pointer phys passes on points into the Multiboot info block, which nothing reserves and which could share that frame (`docs/repairs/261003/x86-phys.md`). The kernel zeroes the slack when it adopts the frames, after ACPI has read the RSDP.

**Change**

```cpp
// before
auto const range = bootmem::Range::from(base_, size_);
// after
auto const range = bootmem::Range::from(base_, ustl::mm::align_up(size_, PAGE_SIZE));
```

**Reference**
- Zircon `MakePhysVmo()` (`zircon/kernel/phys/handoff-prep.cc`): it asserts the address and size are page-aligned and zeroes the partial last page.
- Its kernel insists that reserved ranges be page-rounded (`vm/pmm_node.cc`).

**Verification**
`boot_check.py`: dbg PASS (20 passed), rel PASS (18 passed).

The counts are unchanged, as expected:
- `Reserved 296 frames of boot package` on rel and 315 on dbg, since the stamping already rounded the end up;
- `Released 12 regions, 126491 frames` on rel and 126420 on dbg, since the tail frame stopped going to the buddy in the previous mm fix.

**Deliberately not done**
- Zeroing the slack (above), and the adoption itself: steps 2 to 4 of the plan.

**How it was found**
None: a planned step.

**The user's prediction**
None given.

**Three questions you should be able to answer**
- Why must the image's reservation cover its last frame whole, though the image ends mid-frame?
- Why does phys not zero the slack, as Zircon's phys does?
- Why do the reserved and released frame counts not change with this step?
