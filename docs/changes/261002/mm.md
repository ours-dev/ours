# mm changes

## Title: Kernel placement can hand out the last page of a region (2026-10-02)

**Files**
- `kernel/mm/vm_area_or_mapping.cpp` — `VmaSet::find_spot()`: `GapSeeker::fits()` measures the window's top as the last address it is, and the gap after the last area against the window alone; a request that rounds up to 0 is InvArgs.
- `examples/shell/shell.cpp` — `check_vma_allocate()`, in two 2-page sub-regions of their own: a nested sub-region placed by the kernel fills its parent exactly; with page 0 taken, an anywhere mapping lands on page 1 and the next is refused.

**Symptom**
A mapping or sub-region placed by the kernel (no Specific) that would end on the last page of its region was refused with NotFound, while the same range with Specific was Ok. In an empty 2-page sub-region, `vma_allocate(sub, CanMapRead, 0, 2 pages)` was NotFound; after one anywhere mapping took page 0, a second 1-page anywhere mapping was NotFound with page 1 free. The root regions are large enough to hide it. The CanMap sub-regions added earlier today put it within reach of ring 3, and the review of that change found it.

**Cause**
Two conventions for the end of a range meet in `fits()`, and it measured both the same way.

- An area ends the gap before it with its base: one past the gap's last address, an exclusive end.
- The window's top is passed by `VmArea::create_subaom_preamble()` as `m_base + m_size - 1`, the region's last address, an inclusive end. It has to be: the kernel root VMA ends at 2^64, and one past its end wraps to 0. That wrap is why ad3e5550 moved every extent comparison to last addresses.

`fits()` treated the window's top as exclusive:

```cpp
if (start > m_high_limit || m_high_limit - start < m_requested_size) { // the window
    return false;
}
if (gap_end < start || gap_end - start < m_requested_size) {           // the gap
    return false;
}
...
seeker.fits(seeker.m_prev_end, high_limit) // the gap after the last area: its end is the inclusive top
```

So `[start, start + size)` was taken only if `start + size <= last`: it had to stop at least one byte short of the window's end. Starts are page-aligned and sizes are whole pages, so that one byte costs a whole page, and a free tail of n pages held n - 1. The lost page is the smaller part of the cost. A region sized exactly to its contents, which is what a loader reserves for a program, could never be filled by kernel placement, and a 1-page region could hold nothing the kernel placed.

**Change**
`fits()` takes the area that ends the gap, if there is one, instead of an end that had to mean two things:

```cpp
// before
auto fits(VirtAddr gap_start, VirtAddr gap_end) -> bool {
    ...
    if (start > m_high_limit || m_high_limit - start < m_requested_size) {
        return false;
    }
    if (gap_end < start || gap_end - start < m_requested_size) {
        return false;
    }

// after
auto fits(VirtAddr gap_start, ustl::Option<VirtAddr> next_base) -> bool {
    ...
    if (start > m_high_limit || m_high_limit - start < m_requested_size - 1) {
        return false;
    }
    if (next_base.is_some() &&
        (*next_base < start || *next_base - start < m_requested_size)) {
        return false;
    }
```

The gaps between areas still pass the next area's base. The gap after the last area passes `ustl::NONE`, because the window is all that ends it. `m_requested_size - 1` needs a request of at least one byte. So a size that rounds up to 0, which is any size within one alignment unit of 2^64, is now InvArgs up front. Before, it would have been measured as 2^64 - 1.

Rejected:
- Pass `high_limit + 1` as an exclusive end for the last gap. That wraps to 0 for the kernel root VMA.
- Have the caller pass `m_base + m_size`. That is the same wrap, which ad3e5550 removed.

**Reference**
Zircon `RegionList::FindSpotAtIndex()` (`zircon/kernel/vm/include/vm/vm_address_region.h:413-565`) measures the gap after the last child against `parent_max_byte = parent_base + (parent_size - 1)`, with `remaining_size = parent_max_byte - next_gap_start + 1`. That is an inclusive top, for the same reason. Its `region_list_get_alloc_spot_test` (`zircon/kernel/vm/unittests/aspace_unittest.cc:2389-2396`) allocates the whole tail of a window that ends at 2^64 and expects ZX_OK.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed), rel PASS (13 passed, 10 skipped); `vma_allocate checks passed` in both.
- The new checks, in both logs: `vma_allocate(CanMapRead, anywhere, the whole of its parent) = Ok` at its parent's start; `vma_map(R, anywhere, the sub-region's last page) = Ok` at the sub-region's start plus a page, with page 0 mapped Specific first; then `vma_map(R, anywhere, the sub-region full) = NotFound`.
- Mutation: `vm_area_or_mapping.cpp` put back as it was, the new checks kept. Both images FAIL, each new check on its own: `the whole of its parent) = NotFound (expected Ok)` and `the sub-region's last page) = NotFound (expected Ok)`. Restored; both PASS.
- A first version of the checks mapped page 1 and expected an anywhere mapping to land on page 0. The mutation run showed that check passing without the fix: page 0 there is a gap between areas, which an area's base ends, and that kind of gap was always measured right. It was replaced by the page-1 case above.

**Deliberately not done**
- The answer for no room stays NotFound. Zircon answers ZX_ERR_NO_RESOURCES, and `syscalls.gni` documents neither for `vma_allocate` or `vma_map`. That choice is part of the open question of this ABI's status codes (`.agents/tasks/system-abi-surface/DESIGN.md`), so it is left for the user.
- No unit test of `find_spot()` itself: mm has none to add it to, and the shell's checks reach it through both system calls that place.

**How it was found**
The adversarial review of the `vma_allocate` extension: three reviewers, one per lens, found it independently. One modelled `GapSeeker` in a script, which refused a 2-page request in a 2-page region, a 1-page request in a 1-page region, and a 1-page request in a 2-page region with page 0 taken.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the window's top have to be an inclusive last address, while the end of a gap between two areas can be an exclusive base?
- Why did an off-by-one of a single byte cost a whole page, and why does that matter most to a loader?
- What would `m_requested_size - 1` do if the request were allowed to round up to 0?
