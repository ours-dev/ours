# boot-image-pages: hand userboot the boot image's own pages in a paged VMO

**Date**: 2026-10-03
**Status**: Accepted

## Problem
userboot gets the boot image (the OBI container) as `VmoBootdata`. Today the kernel copies it, about 1.18 MiB, into a fresh committed VMO (`make_boot_image_handle()`, `kernel/lib/userabi/src/userboot.cpp`). The image's own frames stay reserved (BootPackage) for good.

Zircon does neither. `CreatePhysVmo()` (`zircon/kernel/top/handoff.cc`) builds an ordinary paged VMO with `VmObjectPaged::CreateFromWiredPages()`, which takes ownership of the image's frames. Two things follow:
- `vmo_read` works on it;
- the frames return to the page allocator when userboot closes the handle.

The user chose this route ("B") on 2026-10-03. An earlier note claimed Zircon uses a physical VMO here. That was wrong and is corrected in step 4.

## Goals and non-goals
Goals:
- No copy. userboot's VMO owns the image's frames, and closing it frees them.
- userboot itself does not change.
- After the hand-off the kernel never reads the image again, and a late reader fails loudly.

Non-goals:
- The MBI/RSDP hazard found on the way (`docs/repairs/261003/x86-phys.md`).
- The frame-flag overwrite (`docs/repairs/261003/mm.md`, item 1).

## Design
The objects:
- **The boot image.** It sits at a page-aligned base (multiboot enforces that) and is not a page multiple: 1210624 bytes on rel. phys reserves exactly [base, base + size), as BootPackage.
- **Its frames.** `stamp_reserved_frames()` leaves them with role None and flag Reserved, counted on their node and zone.
- **A paged VMO's page map**, `VmCowPages::m_pages`, guarded by the tree's mutex. Allocation fills it with frames that went through `become<VmmBody>()` and are tagged Owned. `~VmCowPages` frees Owned frames with `free_frames()`, which checks nothing about them.

The events at boot, after the change:
1. phys reserves BootPackage rounded out to whole pages, with no zeroing (step 1). No other reservation can then claim the tail frame and be miscounted as BootPackage.
2. The kernel reads what it needs from the image (boot options, kernel package, user options) and then *takes* it: a latch after which `boot_package()` and `kernel_package()` panic (step 3).
3. The kernel zeroes the slack after the image's last byte, through the physmap. This happens after ACPI init, the only reader of the unreserved MBI that might share that page.
4. `VmObjectPaged::create_from_wired_pages()` inserts each frame into a new page map, then un-wires it: clear Reserved, subtract the node and zone counts, `become<VmmBody>()`. On a failed insert, the frames not yet taken stay fully Reserved, and the partly built VMO is dropped after its lock is released (step 4).
5. userboot gets the VMO with `Basic | Read | Map`, reads it exactly as today, and closes it. The frames go to the buddy as ordinary VMO pages.

Concurrency: the new VMO is unpublished while it is filled, so no other thread can reach its page map. The lock is still taken, as Zircon does in `CreateFromWiredPages()`, so that the annotations and the insert path's assumptions hold. The frames are reserved, so no allocator can hand them out meanwhile. The latch is written once, by the boot thread in `userboot_init()`, before any user thread exists.

## Alternatives considered
- **A physical VMO over the image** (`VmObjectPhysical`). Never frees the frames. `vmo_read` and faults are NotSupported on it, so userboot would have to change. Rejected by the user.
- **Keep the copy.** No change, but 1.18 MiB stays reserved for good.
- **Zeroing the slack in phys**, as Zircon's `MakePhysVmo()` does. phys does not know where the MBI is: the RSDP pointer points into it, and nothing reserves it. Zeroing a page the MBI shares would wipe the RSDP before ACPI reads it. The kernel zeroes the slack later instead.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Where the slack is zeroed | (a) the kernel, at adoption, after ACPI init; (b) phys, as Zircon does, after passing it the MBI range | (a) | None given | 2026-10-03, as recommended |
| D2. A boot image base that is not page-aligned | (a) phys panics, as the multiboot view already makes it; (b) also add the Multiboot2 module-align tag; (c) fall back to the copy | (a) | None given | 2026-10-03, as recommended |
| D3. API shape | (a) `boot::take_boot_image()` returns the range, plus a `VmoDispatcher` factory, so top stays independent of kernel/object; (b) Zircon's shape: `end_handoff()` returns the VMO handle | (a) | None given | 2026-10-03, as recommended |
| D4. A failed page-map insert (out of memory) | (a) return the error; (b) assert, as Zircon does | (a) | None given | 2026-10-03, as recommended |
| D5. Extra scope | (a) a `!Reserved` debug check on the runtime free path too; (b) delete `create_committed`, which loses its only caller; (c) a kshell adoption test | (a) and (b), not (c) | None given | 2026-10-03, as recommended |

## Testing and verification
- `boot_check.py` on both images after every step.
- After step 4, in dbg:
  - the BootFS file list is unchanged;
  - the free-frame count in the `aspace ... gone` lines is about 296 frames higher than before;
  - the BootPackage reserved count is 296 lower.
- The `!Reserved` check stays armed throughout.
- A late `boot_package()` call panics: tried once by hand, not kept.

## Open questions
None beyond the decisions above.
