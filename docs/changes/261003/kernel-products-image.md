# kernel-products-image changes

## Title: Both dev images carry the user BootFS as a StorageBootFs item of its own (2026-10-03)

**Files**
- `kernel/user_bootfs.gni` (new) — `user_bootfs()`: an `obi_input()` with no type, which obit packs as a StorageBootFs item, LZ4-compressed, at the BootFS's top level. With `init_startup = true` it also generates `config/init/startup`.
- `kernel/products/image/dev/BUILD.gn` — `user_bootfs("bootfs")` with the shell, the user programs, the vDSO and init's startup list, and the image depends on it.
- `kernel/products/image/dev.dbg_1/BUILD.gn` — the same, with the shell, `fault_probe` and the vDSO.

**Symptom**
None: step 1 of `.agents/tasks/userboot-bootfs/PLAN.md`. The plan moves the work of building the user BootFS from the kernel to userboot, as in Zircon.

**Cause**
The objects:
- The boot image (OBI container), with its items `physload`, `cmdline` and the kernel package.
- The kernel package (StorageKernel). It held everything the image boots, the kernel's own files and user space's alike, plus a whitelist (`config/user_bootfs`) the kernel used to copy the user files out into a BootFS VMO at boot.
- StorageBootFs (`BTSF`): an item type the OBI format and `obit pack` already had, which no image used.

Zircon separates the two from the start: STORAGE_KERNEL holds what the kernel boots from, and STORAGE_BOOTFS holds what user space gets. Its userboot decompresses STORAGE_BOOTFS itself. To get there, the image first needs an item user space can be given whole, with nothing of the kernel's in it.

**Change**
```gn
user_bootfs("bootfs") {
  init_startup = true
  deps = [
    ":shell",
    "//kernel/lib/userabi/vdso(//build/toolchain/ours:user-${target_cpu})",
  ]
  foreach(program, _user_programs) {
    deps += [ "$program(//build/toolchain/ours:user-${target_cpu})" ]
  }
}
```

The item holds what its deps install. Which files user space sees is therefore decided by which item a file is packed into. The kernel package is unchanged in this step, and the kernel still builds the user BootFS from it. Nothing reads the new item yet. That is step 2.

`userboot` is not in the new item: nothing starts it from the BootFS. The kernel-built BootFS has it only because the program is marked user-visible.

**Reference**
- Zircon's ZBI item types `ZBI_TYPE_STORAGE_KERNEL` and `ZBI_TYPE_STORAGE_BOOTFS`.
- userboot finds the latter with `get_bootfs_vmo()` (`src/bringup/lib/userboot/rust/src/bootfs.rs`).

**Verification**
- `obit tree out/bringup.x86/kernel.dev.obi`: four items. The new `[BTSF] bootfs` holds `config/init/startup`, `init`, `shell` and `vdso`, 201484 bytes stored.
- The dbg_1 image's `[BTSF] bootfs` holds `fault_probe`, `shell` and `vdso`, 177961 bytes.
- Both kernel packages are unchanged.
- `boot_check.py`: dbg PASS (20 passed), rel PASS (18 passed). phys and the kernel accept the extra item: `BootObi::from()` counts only StorageKernel items.

**Deliberately not done**
- Reading the item (step 2) and taking the user files out of the kernel package (step 3).
- Real compression. The LZ4 encoder writes literals only (`docs/repairs/260925/obi.md`, item 3), so the item is stored at about its full size.

**How it was found**
None: a planned step.

**The user's prediction**
None given; the user accepted the recommended design (D1 to D3 in the plan).

**Three questions you should be able to answer**
- Why does the user BootFS need an item of its own rather than a list the kernel applies to the kernel package?
- Why is `userboot` in the kernel package but not in the new item?
- Which component decides that a block of files becomes a StorageBootFs item rather than a StorageKernel one?
