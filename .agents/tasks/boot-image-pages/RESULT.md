# boot-image-pages: userboot's boot image VMO owns the image's frames

**Date**: 2026-10-03

## Delivered
- `72e28d8a` fix(phys): the boot image is reserved in whole pages, from a page-aligned base.
- `efb25898` refactor(mm): `unwire_frame()`, and a Reserved check on the runtime free path.
- `ad8a034c` refactor(userabi): the kernel takes the boot image once it has read it; a late reader panics.
- `37ffb76e` feat(mm,object,userabi): `create_from_wired_pages()`. userboot's VMO takes the image's frames, and they go to the buddy when userboot closes it.
- `21b12353` docs(repairs): the MBI item corrected on when ACPI reads the RSDP.

## Verification
- `boot_check.py` after every step: dbg PASS (20 passed), rel PASS (18 passed).
- After step 4:
  - 316 frames (dbg) and 297 frames (rel) are handed over, matching the BootPackage stamping;
  - about 300 to 316 frames more are free once userboot exits;
  - both reserved-frame guards stay armed and quiet.
- A late `boot_package()` panics, shown once by hand in a copy of the tree.
- An adversarial review of step 4 confirmed 7 distinct findings, all fixed before the commit. The one behavioural finding: the VMO allocated with `GAF_KERNEL`, while the `create_committed()` it replaced used `GAF_USER`.

## Deviations from the design
- D4 changed from "return the error" to Zircon's assert, with Zircon's order (unwire, then add). The user asked to stay consistent with Zircon.
- `VmCowPages::add_wired_page_locked` became the general `add_new_page_locked`, Zircon's `AddNewPageLocked()`, after the user asked not to keep adding special page interfaces.
- `create_from_wired_pages()` takes a `Gaf`, which the design did not mention (review finding).

## Found along the way
- The Multiboot info block, where the RSDP pointer points, is never reserved: `docs/repairs/261003/x86-phys.md`, item 1, corrected by the review on when ACPI reads it.
- The frame-flag setters overwrite each other: `docs/repairs/261003/mm.md`, item 1. The flag audit the user asked for is to decide that and the rest of the flags.

## Not done
- A release-build check that each adopted frame is reserved. Zircon asserts it too.
