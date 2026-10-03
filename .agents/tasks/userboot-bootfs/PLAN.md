# userboot-bootfs: three steps from a kernel-built BootFS to userboot's own

**Date**: 2026-10-03
**Status**: Accepted

## Request
The user chose to carry "user space uses nothing on the kernel side but system calls" one step further: as in Zircon, the kernel should hand userboot the boot image, and userboot should unpack the user BootFS itself. The scope ends at the kernel no longer building, filtering or parsing the user BootFS (DESIGN.md, Goals).

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. How the image is split | (a) StorageKernel + StorageBootFs; (b) one package handed to userboot | (a) | None given | 2026-10-03, as recommended |
| D2. What the kernel hands userboot | (a) the whole image copied into a paged VMO; (b) a physical VMO; (c) the BootFS item alone | (a) | None given | 2026-10-03, as recommended |
| D3. Library placement | (a) `//kernel/lib/obi` and `//kernel/lib/lz4` as they are; (b) move to `system/lib` first | (a) | None given | 2026-10-03, as recommended |

## Steps
1. **The image gains a StorageBootFs item.** Every file marked `bootfs_user_visible` is packed, LZ4-compressed, into a StorageBootFs item beside the kernel package. The kernel package is unchanged and nothing reads the new item yet.
   - Why first: the next step needs the item to exist, and this one can be checked on its own with `obit tree`.
   - Verified by: `obit tree` on both images shows the item and its files; `boot_check.py` unchanged.
   - Commit: `feat(image): carry the user BootFS as a StorageBootFs item`.
2. **userboot unpacks the BootFS from the boot image.**
   - The kernel adds `VmoBootdata` to userboot's processargs: a paged VMO holding a copy of the boot image.
   - userboot finds the StorageBootFs item, decompresses it into a VMO, drops `Write`, and uses that VMO for its launcher and the hand-off.
   - It closes the kernel-built `VmoBootfs`, which the kernel still sends for one more step.
   - GN: userboot depends on `//kernel/lib/obi:headers` in the user toolchain; its LZ4 decoder is header-only, so `//kernel/lib/lz4` is not needed.
   - Found while doing it: the BootFS builder aligned file data to 8 bytes, and a slice needs a page. Fixed in its own commit first (`fix(bootfs)`).
   - Verified by: `boot_check.py` on dbg and rel; userboot's new log lines; the shell's `ls` over serial lists the files of the new item.
   - Commit: `feat(userboot): unpack the BootFS from the boot image`.
3. **The kernel stops building the user BootFS.**
   - Delete `make_user_bootfs()`, `user_bootfs.{hpp,cpp}`, the whitelist and `kernel_package()`'s `user_bootfs` option.
   - The user programs leave StorageKernel, which keeps `physboot`, `vmours/*`, `userboot` and `vdso`.
   - The kernel no longer sends `VmoBootfs`.
   - Verified by: `obit tree`; `boot_check.py`; the kernel log has no user BootFS lines; the kernel package's file list in the log shrinks to four entries plus `code-patches`.
   - Commit: `refactor(userabi): the kernel no longer builds the user BootFS`.

## Files
- `kernel/products/image/dev/BUILD.gn`, `kernel/products/image/dev.dbg_1/BUILD.gn` (or the template both use) — the StorageBootFs item; later, user files out of the kernel package.
- `kernel/kernel_package.gni` — the `user_bootfs` option goes (step 3).
- `kernel/lib/userabi/src/userboot.cpp` — `VmoBootdata` in the processargs (step 2); no user BootFS (step 3).
- `kernel/lib/userabi/src/user_bootfs.{hpp,cpp}` — deleted (step 3).
- `kernel/lib/userabi/userboot/userboot.cpp`, `BUILD.gn` — unpack the BootFS (step 2).
- `docs/changes/261003/<module>.md` — one record per step.

## Test plan
- Step 1: `obit tree out/bringup.x86/kernel.dev.obi` lists a `[BTSF] bootfs` item with `init`, `shell`, `vdso` and `config/init/startup`. The dbg image's item also has `fault_probe`.
- Steps 2 and 3:
  - `boot_check.py`: dbg 20 passed, rel 18 passed, `probes done (16 killed, 0 survived)`;
  - `userboot: started shell` on dbg and `userboot: started init` on rel;
  - over serial on rel, the shell's `ls` lists the same files as the item.
