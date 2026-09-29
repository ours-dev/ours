# mm changes

## Title: ZERO_PAGE is allocated zeroed (2026-09-29)

**Files**
- `kernel/mm/init_vmm.cpp` — `init_vmm_preheap()` allocates the zero page with `GAF_KERNEL | Gaf::Zero` and checks in the debug image that it reads as zero.

**Symptom**
Latent, cannot be triggered today: nothing reads `ZERO_PAGE` or `get_zero_page()`, so no one has seen it. It would be triggered by the first reader, for example a copy-on-write child answering a read of a hole with the zero page, which would return whatever the previous owner of the frame left in it.

**Cause**
`Gaf::Kernel` is `ThisNode | Reclaim | Write` (`kernel/mm/include/ours/mm/gaf.hpp`); only `Gaf::Boot` and `Gaf::User` carry `Zero`. The page allocator honours `Zero` in `PmZone::allocate()` (a fill of the whole block after the buddy call), and the buddy allocator gives a frame back exactly as it was freed. So a frame taken with `GAF_KERNEL` alone is not zero, and `init_vmm_preheap()` took it that way and then pinned it as the shared zero page.

It went unnoticed for two reasons that compound. Nothing reads the page, so no behaviour depends on it. And a fresh QEMU guest starts with zeroed RAM, so even a reader would usually see zeros at the first boot and fail only after frames have been freed and reused, which is the kind of failure that appears far from its cause. `VmCowPages::alloc_pages()` already carries a comment about exactly this trap ("`GAF_KERNEL` does not imply it"); this call site was the one that predates it.

**Change**
Before:

```cpp
auto frame = alloc_frames(GAF_KERNEL, usize(0));
```

After:

```cpp
auto frame = alloc_frames(GAF_KERNEL | Gaf::Zero, usize(0));
...
DEBUG_ASSERT(std::all_of(frame_to_virt<isize>(ZERO_PAGE),
                         frame_to_virt<isize>(ZERO_PAGE) + PAGE_SIZE / sizeof(isize),
                         [](isize const word) { return word == 0; }),
             "the zero page is not zero");
```

The assertion is a `DEBUG_ASSERT`, so the release image pays nothing, and it runs once on one page. It is there because a booted kernel is the only evidence a change here can give and nothing else looks at the page.

Rejected: zeroing by hand with `std::fill_n` after the allocation, as Zircon does. `Gaf::Zero` does the same fill in the allocator, and a second spelling of the same fact would drift from it.

**Reference**
Zircon `vm_init_preheap()` (`zircon/kernel/vm/vm.cc:181-190`): `pmm_alloc_page()` for the zero page, `set_state(WIRED)`, then `arch_zero_page()` on its physmap address.

**Verification**
- `scripts/format_cpp.py --check kernel/mm/init_vmm.cpp`: clean. `clang-tidy -p out/vmo-snapshot kernel/mm/init_vmm.cpp`: no diagnostic on the changed lines; the ones reported are in headers this change does not touch.
- `boot_check.py --out out/vmo-snapshot`: both images PASS. dbg: 18 passed, 0 failed, 0 error lines. rel: 13 passed, 9 skipped, 0 failed, 0 error lines.
- The assertion string is present in the `kernel_x86.dbg.1` object (the debug image) and absent from `kernel_x86.dbg.0` (the release one), so the dbg boot did run the check.
- Not shown: the bug itself. A fresh guest has zeroed RAM, so the page was probably zero before this change as well; the boot cannot tell the old code from the new. The change rests on the allocator contract, not on an observed failure.

**Deliberately not done**
- No reader of `ZERO_PAGE` was added. That is the copy-on-write child's cursor, a later step of `.agents/tasks/vmo-snapshot-child/`.
- The `VmmBody` fields of the page (`vmo`, `vmo_index`, `num_mappings`) were not looked at. A page mapped by many address spaces does not fit "one page, one owning VMO", which matters when the first reader maps it.
- The stale comment in `kernel/object/vmo_dispatcher.cpp` ("there is no shared zero page yet") was left; it is a different file and a different subject, and it becomes true or false depending on what the copy-on-write work decides.

**How it was found**
Read while reviewing the design of the copy-on-write child VMO: the design needed "a page that reads as zero", and `grep -rn ZERO_PAGE kernel` showed the definition in `vmm.cpp`, the allocation in `init_vmm.cpp`, the accessor in `vmm.hpp` and no other use. Checking how the frame is allocated against the `Gaf` values then showed the missing `Zero`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `GAF_KERNEL` not imply `Gaf::Zero`, and which two `Gaf` presets do?
- Why could a non-zero `ZERO_PAGE` stay invisible through many boots, and what would the first symptom look like?
- Why is the check a `DEBUG_ASSERT` rather than an `ASSERT`, and what does a passing dbg boot prove and not prove about this fix?
