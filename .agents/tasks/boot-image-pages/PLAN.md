# boot-image-pages: four commits from a copied boot image to its own pages

**Date**: 2026-10-03
**Status**: Proposed

## Request
Route B of the boot-image hand-off, chosen by the user on 2026-10-03: userboot's `VmoBootdata` owns the image's frames, as Zircon's `CreateFromWiredPages()` does, instead of a copy. The scope ends when the copy is gone and the frames return to the allocator after userboot.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1 to D5 | See DESIGN.md | See DESIGN.md | | |

## Steps
1. `fix(phys)`: reserve the boot image in whole pages.
   - `BootPackageInput::from()` refuses a base that is not page-aligned, and reserves `[base, align_up(base + size))`, with no zeroing.
   - Verify: both images boot; the BootPackage stamp count stays 296; the released-frame count is unchanged.
2. `refactor(mm)`: one un-wire helper, and the free path checks it.
   - An `unwire_frame()` helper: assert Reserved, role None, not linked; then clear Reserved and subtract the node and zone counts.
   - `reclaim_init_area()` uses it.
   - A `!Reserved` debug assertion in `PmNode::free_frames()`.
   - Correct the "plain" comment on `sub_reserved_frames()`: the counters are atomic.
   - Verify: dbg boots with both assertions armed.
3. `refactor(userabi)`: the kernel takes the boot image.
   - Read the user options before the boot image handle is made.
   - A latch after which `boot_package()` and `kernel_package()` panic.
   - The kernel still copies, now from the taken range.
   - Verify: both images boot, with the environment lines unchanged.
4. `feat(mm,object,userabi)`: adopt the frames.
   - `VmObjectPaged::create_from_wired_pages()`, inserting before un-wiring, and a `VmoDispatcher` factory.
   - The kernel zeroes the slack, adopts the frames, and logs how many it handed over.
   - Delete `create_committed` if nothing else calls it.
   - Correct the comments that called Zircon's VMO physical: `userboot.cpp`, `.agents/tasks/userboot-bootfs/DESIGN.md`, D2.
   - Verify: as in DESIGN.md's testing section.

## Files
- `kernel/phys/include/ours/phys/boot_package_input.hpp` — step 1.
- `kernel/mm/early_mem.cpp`, `kernel/mm/pmm.cpp` or `pm_node.cpp`, `kernel/mm/include/ours/mm/pm_zone.hpp` — step 2.
- `kernel/top/handoff.cpp`, `kernel/include/ours/boot/handoff.hpp`, `kernel/lib/userabi/src/userboot.cpp` — steps 3 and 4.
- `kernel/mm/vm_object_paged.cpp`, `kernel/mm/vm_cow_pages.cpp` and their headers, `kernel/object/vmo_dispatcher.{hpp,cpp}` — step 4.
- `docs/changes/261003/<module>.md` — one record per step.

## Test plan
As DESIGN.md's testing section, after each step.
