# userboot-bootfs: userboot unpacks the user BootFS, the kernel stops building it

**Date**: 2026-10-03
**Status**: Accepted

## Problem
The user's rule is that once user space runs, it uses nothing on the kernel side but system calls. After K25 the kernel loads no program but userboot. It still does one piece of user space's work, though: it builds the BootFS user space is given.

Today the boot image has one kernel package, the StorageKernel item. That item holds everything:
- the kernel's own files: `physboot`, `vmours/image.elf`, `vmours/code-patches`;
- `userboot` and `vdso`;
- the user programs: `init`, `shell`, and `fault_probe` on the dbg image;
- `config/init/startup`, and `config/user_bootfs`, a whitelist generated from GN metadata.

In `userboot_init()` the kernel does three things with it:
1. It decompresses the whole package into a buffer it never frees (about 1 MiB).
2. It reads the whitelist and copies the whitelisted files into a fresh BootFS VMO (`make_user_bootfs()`, `user_bootfs.cpp`).
3. It hands that VMO to userboot.

So the kernel parses BootFS, applies a policy of what user space may see, and lays out a filesystem image. In Zircon all of that is userboot's: the kernel hands userboot the ZBI as a VMO, and userboot decompresses the STORAGE_BOOTFS item itself (`get_bootfs_vmo()`).

## Goals and non-goals
Goals:
- The kernel never builds, filters or parses the user BootFS.
- userboot finds the BootFS item in the boot image and decompresses it into a VMO. It uses that VMO for its own launcher, and hands it on read-only.
- What user space sees is decided at build time, by which item a file is packed into. The whitelist file and the kernel code that applies it go.

Non-goals:
- Freeing the kernel's decompressed package (`docs/repairs/261003/userabi.md`). This design shrinks it to the kernel's own files but does not remove it. The kernel still needs `userboot` and `vdso` from it. Reusing the copy phys already decompressed is a separate change.
- A real LZ4 encoder. Today's encoder writes literals only (`docs/repairs/260925/obi.md`, item 3), so "decompressing" is a copy until that lands. The decoder is a real LZ4 block decoder, and nothing here changes when the encoder does.
- Moving `//kernel/lib/obi` or `//kernel/lib/lz4` to `system/lib`.

## Design
The objects:
- The boot image (OBI container). It lives in physical memory the kernel reaches through the physmap (`boot::boot_package()`). Its items include `physload`, `cmdline` and the kernel package.
- **StorageKernel** (`KSTR`): the kernel package. After this change it holds `physboot`, `vmours/*`, `userboot` and `vdso`. phys loads the kernel from it, and the kernel loads userboot and lays out the vDSO from it.
- **StorageBootFs** (`BTSF`), new in the image: the user BootFS. It holds `init`, `shell`, `fault_probe`, `vdso` and `config/init/startup`, and is LZ4-compressed like the kernel package. `obit pack` already emits this item type (its default `files_type` is `bootfs`). phys's `BootObi::from()` counts only StorageKernel items, so an extra item is accepted.
- `HandleType::VmoBootdata` (0x1a, Zircon's `PA_VMO_BOOTDATA`), already in `<ours/process_args.hpp>`: the boot image as a VMO, handed to userboot.
- The decompressed BootFS VMO, made by userboot and handed on as `VmoBootfs` with today's rights (`Basic | Read | Map | GetProperty`, no `Write`).

The events at boot, after the change:
1. phys loads the kernel from StorageKernel, as today.
2. The kernel decompresses StorageKernel, lays out the vDSO and launches userboot, as today.
3. The kernel copies the boot image into a paged VMO and puts it in userboot's processargs as `VmoBootdata`. It no longer sends a `VmoBootfs`.
4. userboot parses the VMO with `obi::ContainerView` and finds the StorageBootFs item.
5. userboot creates a VMO of the item's `memory_size`, maps it read-write, and decompresses into the mapping with `obi::ItemView::materialize()`, whose LZ4 decoder is header-only (`obi/compression.hpp`).
6. userboot makes a handle to that VMO without `Write` (`handle_duplicate`) and closes the writable one.
7. userboot builds its `BootfsLauncher` from the read-only handle, launches `userboot.next`, and hands the handle on as `VmoBootfs`. It closes the boot image VMO and exits; the read-write mapping goes with it.

Why `Write` must go before the hand-off: every program's code is mapped from slices of this VMO, through an executable duplicate of it. A holder with `Write` could change the code of every program started from it.

Zircon counterparts:
- `zircon/kernel/top/handoff.cc`: `.zbi = CreatePhysVmoHandle(...)`, the ZBI as a VMO;
- `zircon/kernel/lib/userabi/userboot.cc`: pushes that handle to userboot;
- `src/bringup/lib/userboot/rust/src/bootfs.rs`, `get_bootfs_vmo()`: create, map read-write, decompress, unmap.

## Alternatives considered
- **One package, handed whole to userboot, which filters it.** Rejected. User space would receive the kernel's own image, and the whitelist logic would move instead of disappearing. Zircon keeps the two apart as STORAGE_KERNEL and STORAGE_BOOTFS.
- **A physical VMO over the boot image's memory**, as Zircon's `CreatePhysVmoHandle()` does. No copy, but:
  - `vmo_read` answers NotSupported on a physical VMO here, so userboot would have to map it, and mapping a physical VMO from user space is untested;
  - the image's pages would have to stay reserved for good.

  A copy of about 1.2 MiB at boot is cheaper to get right. It can become a physical VMO later without changing userboot.
- **Only the StorageBootFs item as the VMO.** The kernel would still parse the image to cut the item out, and userboot would need the item's compression and size from a side channel. The whole image describes itself, which is why Zircon hands over the whole ZBI.
- **Decompressing into a buffer and `vmo_write`-ing it.** userboot has no allocator, and a static buffer would have to be as large as any BootFS.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. How the image is split | (a) StorageKernel + StorageBootFs; (b) one package handed to userboot | (a), Zircon's shape | None given | 2026-10-03, as recommended |
| D2. What the kernel hands userboot | (a) the whole image, copied into a paged VMO, as VmoBootdata; (b) a physical VMO over the image; (c) the BootFS item alone | (a) now, (b) later if wanted | None given | 2026-10-03, as recommended |
| D3. Where userboot's OBI parser and LZ4 decoder come from | (a) `//kernel/lib/obi` headers and `//kernel/lib/lz4`, built for the user toolchain as they are; (b) move both to `system/lib` first | (a): both are freestanding, and userboot lives in `kernel/lib/userabi` already | None given | 2026-10-03, as recommended |

## Testing and verification
- After each step, `boot_check.py` on dbg and rel, with the same probe, launcher and object_get_info verdicts as today.
- `obit tree` on both images shows what each item holds.
- New log lines from userboot: `userboot: bootfs from the boot image, <n> bytes` and the file count.
- When the kernel stops building it, the kernel's own log no longer mentions the user BootFS or the whitelist.
- Cannot be verified yet: real compression. The encoder writes literals, so the decompression path runs, but on literals only. LZ4's back-references get no coverage until `obi.md` item 3 lands; the decoder's host tests (`lz4-wrapper-smoke`) cover them.

## Open questions
- Does the image's StorageBootFs need `userboot` in it? Nothing launches userboot from the BootFS, so this design leaves it out. Today's user BootFS has it only because it is marked user-visible.
