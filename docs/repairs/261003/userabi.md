# userabi outstanding repairs

## 1. The decompressed kernel package is never freed, though nothing reads it once userboot has started

**Severity**
Low: about 1 MiB of kernel memory held for the life of the system, not a wrong result.

**Symptom**
`userboot_init()` decompresses the kernel package into a buffer from `new (mm::GAF_KERNEL) u8[]`, and that buffer is never released. On the bringup image it holds nine files, about 1 MiB: `vmours/image.elf` alone is 684 KiB, plus `physboot`, `shell`, `init`, `userboot` and the vDSO. The boot log lists them as `userboot:   <name> (<bytes> bytes)`.

**Cause**
The buffer was kept on purpose while `process_spawn_boot` could start a program from the package at any time. That call went at K25 (`docs/changes/261003/syscall.md`), and with it the last reader after boot:
- the user BootFS is a copy (`make_user_bootfs()`, a committed VMO);
- the vDSO's VMO is a copy (`lay_out_vdso()`);
- `SharedVdso::file`, a view into the buffer, is read only by `launch()` when it binds userboot.

So the buffer could go as soon as userboot has started. It is a vmalloc allocation, though, and `vfree()` is not yet safe on more than one CPU: it unmaps with a local `invlpg` only, so another CPU may keep a stale translation to frames that have gone back to the allocator. That is item 2 of `docs/repairs/260908/heap.md`, and this item depends on it.

**Change**
Once `vfree()` shoots the range down on every CPU, free the buffer at the end of `userboot_init()`, after `launch()` has returned and before anything else can run. Then clear `SharedVdso::file`, and assert in `launch()` that it is set, so that a second `launch()` fails loudly instead of reading freed memory.

Ruled out: freeing it now. A use after free on another CPU's stale TLB entry would show up as corrupted memory far from its cause.

**Reference**
Zircon's kernel never holds a decompressed BootFS: it hands userboot the ZBI as a VMO, and userboot decompresses the BootFS itself (`get_bootfs_vmo()` in `src/bringup/lib/userboot/rust/src/bootfs.rs`, called from `zircon/kernel/lib/userabi/userboot/main.rs`).

**Acceptance**
- `userboot_init()` frees the buffer, and `vmalloc_live_ranges()` after boot is one less than before.
- Both images boot under `ours-verify`.
