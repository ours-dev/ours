# bootfs changes

## Title: The BootFS builder aligns every file's data to a page, as Zircon's format requires (2026-10-03)

**Files**
- `system/lib/bootfs/include/bootfs/builder.hpp` — `bootfs::ALIGNMENT` is 4096, not 8. It is where every file's data starts, the value written into each entry's `alignment`, and what the BootFS is padded out to.

**Symptom**
With step 2 of `.agents/tasks/userboot-bootfs/PLAN.md` in place, userboot unpacks the image's StorageBootFs item and launches `userboot.next` from it. Both images then stopped at:

```text
userboot: launch failed: slicing the program out of the BootFS: InvArgs
userboot: starting the next program = InvArgs
```

**Cause**
The objects:
- `bootfs::Builder`: lays out a BootFS. Only `obit pack` uses it, for every BootFS item of the boot image.
- The launcher, `launch::launch_from_bootfs()`. It gives a program its file VMO by slicing it out of the BootFS VMO (`vmo_create_child(Slice, data_offset, data_size)`).
- A slice. Its offset must be page-aligned (`mm::VmObjectPaged::create_child_slice()`), or the call answers InvArgs.

The builder placed each file's data at the next multiple of 8 bytes, so almost no program began on a page and none could be sliced. This went unseen because user space never got a BootFS the builder had laid out. The kernel built the user BootFS itself, from the kernel package, with its own layout. That layout put every file on a page boundary (`DATA_ALIGNMENT = PAGE_SIZE` in `user_bootfs.cpp`) for exactly this reason. phys and the kernel only read file bytes out of the kernel package, which works at any alignment.

Zircon's BootFS format makes page alignment part of the format: every file image is aligned to `ZBI_BOOTFS_PAGE_SIZE` from the start of the payload (`lib/zbi-format/internal/bootfs.h`). That is what lets a program's VMO be a slice of the BootFS VMO there too.

**Change**

```cpp
// before
inline constexpr auto ALIGNMENT = u64{8};
// after
inline constexpr auto ALIGNMENT = u64{4096};
```

The cost is padding: each file is padded out to a page. On the rel image the user BootFS item grows from 201484 to 217941 bytes, and the kernel package from 1033486 to 1060914.

**Reference**
- Zircon `ZBI_BOOTFS_PAGE_SIZE` and `ZBI_BOOTFS_PAGE_ALIGN()` in `sdk/lib/zbi-format/include/lib/zbi-format/internal/bootfs.h`.

**Verification**
- Alone, in a copy of HEAD with only this file changed:
  - `boot_check.py` dbg PASS (20 passed), rel PASS (18 passed);
  - `obit list` shows the larger items above.
- The OBI host tests all pass: `boot-options-smoke`, `cli-smoke`, `container-view-test`, `lz4-wrapper-smoke`, `memory-config-smoke`, `obi-smoke`, `package-builder-test` and `parser-fuzz-smoke`. The `pack-smoke` and `differential-smoke` stamps are rebuilt.
- With step 2 on top, both images launch from the unpacked BootFS.

**Deliberately not done**
- A per-item alignment, page only for the user BootFS. The format is Zircon's, and one rule for every BootFS keeps `bootfs::View` and its readers simple.

**How it was found**
Booting step 2. The launcher's report named the step (`slicing the program out of the BootFS`), and `vmo_create_child`'s comment lists the page-aligned offset as the slice's InvArgs case. `bootfs/builder.hpp` showed the 8-byte alignment.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must a file in the BootFS start on a page for the launcher to start it?
- Why did the 8-byte alignment never fail before?
- What does page alignment cost, and where does that cost show up?
