# userboot-bootfs: userboot unpacks the user BootFS; the kernel builds none

**Date**: 2026-10-03

## Delivered
- `43bc876a` feat(image): the boot image carries the user BootFS as a StorageBootFs item (step 1).
- `43f231ff` fix(bootfs): the BootFS builder aligns file data to a page, as Zircon's format requires; found in step 2.
- `edb86988` feat(userboot): the kernel hands userboot the boot image as `VmoBootdata`, and userboot unpacks the BootFS from it (step 2).
- This commit: the kernel no longer builds the user BootFS, and its package holds only what it boots from (step 3).

## Verification
- `boot_check.py` after every step:
  - dbg PASS, 20 passed: 16 probes killed with -1028, launcher and object_get_info checks passed;
  - rel PASS, 18 passed.
- After step 3:
  - the kernel package has five files, and the user BootFS item four on rel and three on dbg_1;
  - the shell's `ls` over serial lists the item's files;
  - the autorun build (`out/autorun-launch`) passes and starts its autorun program.
- The OBI host tests pass with the page-aligned builder.
- Not measured: LZ4 back-references in the unpack path. The encoder wrote literals only (`docs/repairs/260925/obi.md`, item 3). Since fixed: obit compresses with upstream LZ4, and both images boot through the real decoder.

## Deviations from the design
- userboot needs only `//kernel/lib/obi:headers`, not `//kernel/lib/lz4`: `obi::ItemView::materialize()` decodes LZ4 itself.
- The BootFS builder's 8-byte data alignment had to become a page first. That was not in the plan. It is Zircon's rule (`ZBI_BOOTFS_PAGE_SIZE`), and without it no program could be sliced out of a BootFS the builder laid out.

## Found along the way
- The kernel decompresses its package although phys already has: updated in `docs/repairs/261003/userabi.md`, item 1, with that as the preferred fix.

## Not done
- Handing over the image without a copy, left for later. Done since by `.agents/tasks/boot-image-pages`, as Zircon does it: a paged VMO that takes the image's frames, not D2's option (b), a physical VMO.
- Unmapping userboot's read-write BootFS mapping: there is no `vma_unmap`. It goes when userboot exits.
