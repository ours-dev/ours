# bootfs changes

## Title: bootfs::View refuses file data that starts off a page or shares a page with another file (2026-10-04)

**Files**
- `system/lib/bootfs/include/bootfs/view.hpp` — `View::from()` enforces the page rule: an entry whose data starts off a page is InvArgs, and so are two entries that put bytes on a common page without having the same offset and size. The pair test is the new private `share_pages_partially()`.
- `system/lib/bootfs/include/bootfs/format.hpp` — `bootfs::ALIGNMENT` moves here from `builder.hpp`, with the page rule and its reason stated beside it, since the reader now needs it as much as the writer.
- `system/lib/bootfs/include/bootfs/builder.hpp` — loses the definition of `ALIGNMENT`, which it still uses through `format.hpp`.
- `system/lib/bootfs/test/view_test.cpp`, `system/lib/bootfs/test/BUILD.gn` — new host gtest `view`: each refusal, a zero-size entry, one file under two names, adjacent pages, and a Builder round trip.
- `system/lib/bootfs/BUILD.gn` — a `tests` group.
- `BUILD.gn` — `//system/lib/bootfs:tests` in the host test list.

**Symptom**
Latent, cannot be triggered today. Every BootFS in the tree is laid out by `bootfs::Builder` (through `obit pack`), which starts every file on a page and pads it with zeros to the next one. It would show with a BootFS laid out by anything else: a hand-made image, another packer, or a corrupted item. `View::from()` accepted such an image, and once the root slices one VMO per file and makes the slices under `bin/` executable (D15, step 13), a file's slice would carry its neighbour's bytes, executable along with the file.

**Cause**
The objects:
- A BootFS: a header, an entry table, a name table, and the files' data. Each entry gives a file's name, data offset, data size and an `alignment` field.
- `bootfs::View::from()`: the one parser every reader goes through. Today that is phys's kernel package loader (`kernel/phys/kernel_package_loader.cpp`) and module loader, userboot, init, the launcher, the dbg_1 shell, and `obit tree` on the host.
- A VMO slice (`vmo_create_child` with Slice, `system/public/ours/syscalls.gni:833-852`): a window onto the parent's pages. Its offset must be page-aligned (InvArgs otherwise), and its size rounds up to a page, so it is always made of whole pages.
- `vmo_replace_as_executable`: gives a handle Execute over everything the VMO covers, which for a slice is every one of its pages.
- The root under D15 (a): at ingest it makes one slice per BootFS file at the file's page-rounded range, and replaces the slice with an executable one when the file is under `bin/`.

The events that would go wrong without the rule:
1. A BootFS holds `bin/a` of 5000 bytes at page P, and `data/key` at P + 5000, right after it. The entry claims `alignment = 8`, and P + 5000 is a multiple of 8.
2. `View::from()` checked `data_offset % entry.alignment`, which passes, and the bounds, which pass.
3. The root slices `bin/a` at [P, P + 8192): the slice is two whole pages, and the second also holds the first 3192 bytes of `data/key`.
4. The root makes that slice executable, and hands it to anyone whose namespace has `/boot/bin` with rx rights. `data/key`'s bytes can now be mapped, read and executed through `bin/a`'s VMO, though nothing ever offered `data/key`.

Two lessons sit in this:
- The `alignment` field is a claim the image makes about itself. Checking an offset against it checks only that the image is consistent with its own claims, not that it is safe to slice; an image with `alignment = 1` passes any offset. The page is the reader's own rule, because it comes from what the reader does next (slice at page granularity), not from what the image says. This is why the new check uses `bootfs::ALIGNMENT`, the format's page, and not the entry's field.
- Page granularity turns "where the bytes are" into "where the pages are". Two files that never share a byte can still share a page if one starts off a page, and that is the case (step 1) the first half of the rule removes. Once every file starts on a page, two files can share a page only by sharing bytes: a file that starts at or after another's last byte starts on a page boundary, so at or after the end of that byte's page. That is why `share_pages_partially()` compares the exact byte ranges and never rounds a size up. The rounding would add nothing, and could overflow.

What stays allowed, and why:
- One file under two names (the same offset and the same size). The pages are the same, so neither name exposes bytes the other does not. Fuchsia's BootfsSvc shares one VMO between such entries (`src/sys/component_manager/src/bootfs.rs:262-288`); ours gives each name its own slice of the same pages.
- An empty file at any page-aligned offset, even one inside another file. It covers no page, so it shares none. The Builder gives an empty file the offset of the file laid out after it, so without this exception every image with an empty file would be refused. The kernel package holds one today, `vmours/code-patches`, 0 bytes.

Why it was not caught earlier: before 2026-10-03 nothing sliced a BootFS file. phys and the kernel only read a file's bytes, and reading works at any alignment. When the launcher began to slice programs out of the BootFS, the fix went into the writer (the Builder's `ALIGNMENT` became 4096, `docs/changes/261003/bootfs.md`), and the reader kept trusting whatever the image said. A rule that only the writer keeps is a convention, not a check: a reader that slices must refuse what it cannot slice safely.

**Change**
The first half, in the per-entry loop of `View::from()`, after the bounds check:

```cpp
// before: only the entry's own claim
entry.data_offset % entry.alignment != 0
// after: also the format's page
if (entry.data_offset % ALIGNMENT != 0) {
    return ustl::err(ours::Status::InvArgs);
}
```

The second half, in the same loop: each entry, once checked alone, is compared with every entry before it, so each pair is compared once and both sides of the pair have been checked when it is:

```cpp
for (u32 earlier = 0; earlier < index; ++earlier) {
    Entry other{};
    std::memcpy(&other, entries + earlier, sizeof(other));
    if (share_pages_partially(entry, other)) {
        return ustl::err(ours::Status::InvArgs);
    }
}
```

```cpp
static auto share_pages_partially(Entry const &left, Entry const &right) -> bool {
    if (left.data_size == 0 || right.data_size == 0) {
        return false;
    }
    if (left.data_offset == right.data_offset && left.data_size == right.data_size) {
        return false;
    }
    auto const left_end = left.data_offset + left.data_size;
    auto const right_end = right.data_offset + right.data_size;
    return left.data_offset < right_end && right.data_offset < left_end;
}
```

InvArgs, like every other malformed entry, so `make_bootfs_launcher()`'s documented answer ("InvArgs or OutOfRange for a BootFS that View refuses") still holds.

`bootfs::ALIGNMENT` moved from `builder.hpp` to `format.hpp`: `view.hpp` must not include `builder.hpp`, which needs `std::vector` and `std::string` and so builds only on the host, while View runs in phys and freestanding user code. The name stays, so every existing use compiles unchanged. A `bootfs::PAGE_SIZE` was rejected: `kernel/mm/include/ours/mm/cfg.hpp` defines a `PAGE_SIZE` macro, and `ninja -t deps` shows it in the same translation unit as `bootfs/format.hpp` in phys's module loader and the kernel's `userboot.cpp`, among others, where it would expand inside the declaration.

Where this departs from step 2 of the plan:
- The plan refuses "two entries whose page-rounded ranges overlap without being identical", which could be read as accepting two entries with the same page-rounded range and different sizes, say 100 and 200 bytes at the same offset. This change refuses them: "identical" is read as the same offset and the same size. The 100-byte file's slice would hold the other file's bytes 100 to 199, which is the leak the rule exists to stop; Zircon's format allows sharing only at "the same offset and size"; and BootfsSvc, which the plan cites for sharing, asserts equal sizes on exactly this case (`bootfs.rs:269`). The test `RefusesTwoSizesOnOnePage` pins it.
- The plan lists `view.hpp` and `BUILD.gn` as the files changed; `format.hpp` and `builder.hpp` change too, to give View the page constant without a second copy of it.

Rejected for the pair test:
- Rounding each size up to a page and comparing page ranges. Equivalent once offsets are on pages, as above, but it needs an overflow check the byte comparison does not.
- Sorting the entries by offset and comparing neighbours. View has no allocator in phys and user code, and the format does not order entries by offset. The quadratic cost is recorded as a repair item instead (`docs/repairs/261004/bootfs.md`, item 1).

**Reference**
- Zircon's format states both halves. `zbi_bootfs_dirent_t::data_off` "must be a multiple of ZBI_BOOTFS_PAGE_SIZE", and "Each range of pages can be used for multiple files, but all those files must have the same offset and size, that is, no partial sharing of ranges." (`sdk/lib/zbi-format/include/lib/zbi-format/internal/bootfs.h:43-44`, `:57`).
- Zircon's parser, `zbitl::BootfsView<>::iterator::Update()` (`src/lib/zbitl/include/lib/zbitl/items/bootfs.h:315-376`), refuses an entry with "file offset is not a multiple of ZBI_BOOTFS_PAGE_SIZE", and one whose page-rounded end passes the payload. It leaves "no partial sharing" to the format's promise and checks no pair.
- Fuchsia's component_manager, `aligned_range()` (`src/sys/component_manager/src/bootfs.rs:88-95`), refuses a misaligned offset with `BootfsError::MisalignedOffset`. `BootfsSvc::ingest_bootfs_vmo()` (`:262-288`) keys a `HashMap` of seen ranges by the page-aligned range and shares one VMO between entries that land on the same one, asserting `content_size == entry.size`. That assert is the Zircon rule's "same offset and size", enforced by a panic. Ours refuses the image at parse time instead, before anything is sliced.

**Verification**
- New host test `out/bringup.x86/host/obj/system/lib/bootfs/test/view`: 9 tests, `[  PASSED  ] 9 tests.`
- The same test compiled against the old `view.hpp` (HEAD's, with this change's `format.hpp` and `builder.hpp`): the five refusal tests fail and the four acceptance tests pass, so each refusal test depends on the new rule.
- `boot_check.py`: dbg PASS (24 passed, 1 skipped, 0 failed), rel PASS (22 passed, 3 skipped, 0 failed). Every View user parses with the rule on the way: phys's loaders, userboot ("kernel package holds 5 files", including the empty `vmours/code-patches`), init, the launcher and the dbg_1 shell.
- `obit tree out/bringup.x86/kernel.dev.obi` lists both BootFS items, and the OBI host tests (`cli-smoke`, `obi-smoke`, `package-builder-test` and the rest) all exit 0.
- clang-tidy on `view_test.cpp`, with the bootfs headers in the header filter: no diagnostic on a changed line. The ones it reports (`data_`, `files_` and the other members not in `m_` style) predate this change.

**Deliberately not done**
- Checking that a file's page-rounded end stays inside the BootFS, as zbitl does. The root slices the BootFS VMO, whose size is the BootFS rounded up to a page, so a rounded end past `total_size` is still inside the VMO and reads zeros; and the Builder pads `total_size` to a page anyway. The plan named two refusals, and this one protects nothing today.
- Checking that the padding after a file's last byte is zero. Under the rule, nothing else lives on that page, so the padding is no other file's bytes; what it holds is the image's own business.
- Removing the per-entry `alignment` field, which the format's page now makes redundant. It would change the on-disk format and `obit`; not this step's scope.
- Sorting or indexing the entries to make the pair check linear: see the repair item.

**How it was found**
None. This is step 2 of `.agents/tasks/component-framework/PLAN.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is checking `data_offset % entry.alignment` no protection against a file's slice exposing a neighbour's bytes, while `data_offset % bootfs::ALIGNMENT` is?
- Once every file starts on a page, why does comparing exact byte ranges decide whether two files share a page?
- Why may two entries with the same offset and the same size share pages, but not two with the same offset and different sizes, and what would go wrong under D15 if the second were allowed?
