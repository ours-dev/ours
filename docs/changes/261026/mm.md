# mm changes

## init_memmap: the hole mark follows the cursor instead of jumping to the region end (2026-10-26)

**Files**
- `kernel/mm/memmap_init.cpp` — `hole_start_pfn` now takes the cursor `init_memmap_zone()` returned, not `end_pfn`

**Symptom**
Latent, cannot be triggered today as a visible failure: both images boot and the shell's self-checks pass either way. What it leaves behind is a set of frame descriptors that were never initialised at all — all-zero, so `role()` is `None`, `secnum()` is 0, and `Flags::Initialized` is clear — while `pfn_valid()` answers true for them. Those descriptors reach the buddy allocator, and `FrameBuddy::is_mergeable()` checks only `role`, `order`, `zone` and `nid`, so a zero descriptor whose `role` happens to read as something it accepts is taken for a free buddy. The trap it ends in is `ustl::collections::intrusive::detail::require()`, not a bounds check, so the symptom when it does fire is a bare `ud2` inside `FrameBuddy::release()` with no message.

**Cause**
`init_memmap()` walks each usable region, and inside a region walks that node's zones in type order — Dma, then Dma32, then Normal. Two cursors move through that walk. `start_pfn` is how far descriptors have been written, and `init_memmap_zone()` maintains it honestly: it returns `last` when it initialised `[first, last)`, and returns `start` **unchanged** when the zone does not reach what is left of the region, so that a zone which did nothing does not move the caller on. `hole_start_pfn` is the other cursor, and it is what tells the next zone pass where the gap in front of it begins, so that `[hole_start, first)` can be given reserved descriptors.

The second cursor was not maintained honestly. It was assigned `end_pfn` — the end of the whole region — after every zone pass, whether or not that pass wrote anything. For a region that lies entirely above the Dma zone, which is every region on a machine with more than 16 MiB of RAM, the sequence is: the Dma pass clamps `[start, end)` into the Dma zone, finds `first >= last`, initialises nothing and returns the cursor unmoved — and then the loop body advances the hole mark to the end of the region anyway. The Dma32 pass that actually covers the region then evaluates `if (hole_start < first)` with `hole_start` already at or past `first`, concludes there is no hole in front of it, and skips `init_memmap_reserved()` entirely.

So the gap between one region and the next never got its reserved descriptors. This is not a small window: it is every hole in the physical memory map below the last usable region, on every boot. The reason it stayed invisible is that `stamp_reserved_frames()`, which runs afterwards and would otherwise have marked them, skips any descriptor without `Flags::Initialized` — so the one pass that could have caught the omission is also the one the omission hides from.

The deeper lesson is about which of two cursors is the source of truth. `init_memmap_zone()` was already written to report "I did not move", and its comment explains at length why it returns `start` and not the clamped `first`. The caller then threw that report away for the second cursor while honouring it for the first. Two cursors tracking the same progress, only one of them fed from the function that knows the progress, is the bug.

**Change**
Before:

```cpp
node->zone_list()->for_each_local_zone([&](PmZone &zone) {
    start_pfn = init_memmap_zone(zone.zone_type(), zone.start_pfn(), zone.end_pfn(),
                                 start_pfn, end_pfn, hole_start_pfn, region.node);
    hole_start_pfn = end_pfn;
});
```

After:

```cpp
node->zone_list()->for_each_local_zone([&](PmZone &zone) {
    start_pfn = init_memmap_zone(zone.zone_type(), zone.start_pfn(), zone.end_pfn(),
                                 start_pfn, end_pfn, hole_start_pfn, region.node);

    // The cursor, not the region end. The hole mark says how far
    // descriptors actually reach, and a zone that does not reach
    // what is left of this region hands the cursor back unmoved --
    // so it must not move either.
    hole_start_pfn = start_pfn;
});
```

One line. `start_pfn` is the value `init_memmap_zone()` just returned, so the hole mark is now unchanged exactly when the zone pass did nothing, and equal to `last` exactly when it initialised up to there. Across regions it therefore ends a region at the last initialised pfn, which is what makes the next region's first covering zone see the inter-region gap and mark it.

Rejected: passing `hole_start_pfn` by reference into `init_memmap_zone()` so the callee maintains it. That would put two out-parameters on a function that already returns its cursor, and would spread the invariant across the call boundary instead of keeping it where both cursors are visible at once. Also rejected: deriving the hole mark from `zone.end_pfn()` of the previous zone, which is wrong for a region that starts partway into a zone.

**Reference**
Linux `memmap_init()` in `mm/mm_init.c` keeps the same two pieces of state, as `hole_start_pfn` and the per-zone range, and advances the hole only from what `memmap_init_zone_range()` reports back through its `hole_pfn` out-parameter — never from the end of the memblock region. `init_unavailable_range()` is the counterpart of `init_memmap_reserved()`.

**Verification**
`python3 scripts/format_cpp.py --check kernel/mm/memmap_init.cpp` clean. `prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86 kernel/mm/memmap_init.cpp` reports nothing on the changed lines. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lane.memmap`:

```text
build      ok (out/lane.memmap, 24.3s)
dbg        PASS  shell finished at 5.6s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end
rel        PASS  shell finished at 4.6s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end
host-tests PASS  147 passed, 0 failed · 10.7s
```

Not measured: whether any descriptor was actually left uninitialised before this change, and how many. That would need a walk over the memmap counting descriptors without `Flags::Initialized`, which does not exist yet. The boot result here establishes no regression, not that this change fixed an observed failure — the tree was already booting when the change was made.

**Deliberately not done**
`FrameBuddy::is_mergeable()` still does not check `Flags::Initialized` or `Flags::Reserved`, so an uninitialised descriptor reaching it from any other source is still accepted. That is the second half of the same failure and is filed separately in `docs/repairs/261026/mm.md` item 6, because it is a defence worth having on its own rather than something this fix makes unnecessary.

**How it was found**
A subagent scan of `kernel/mm/` for the pattern "framework written, one step never landed" turned up the assignment as a cursor overwritten inside a loop it should have been outside of. Confirmed by reading `init_memmap_zone()`'s two return paths: the `first >= last` path returns `start` with a comment explaining that returning the clamped value walked the cursor backwards, which establishes that "the cursor did not move" is a state the function deliberately reports. Then by reading `stamp_reserved_frames()`, which skips descriptors without `Flags::Initialized` and so cannot compensate. The earlier `ud2` that prompted the scan was in `FrameBuddy::release_inner()`, a function since rewritten away, so it is not evidence for this item.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `init_memmap_zone()` returns `start` rather than the clamped `first` when the zone does not reach the region. What breaks if it returns `first` instead, and why is that worse than returning nothing at all?
2. The omission left descriptors with `Flags::Initialized` clear. Name the two later passes that each had a chance to notice, and say why neither did.
3. Two cursors tracked progress through the same walk. Which one was the source of truth, how would you tell from the code alone, and what rule would have prevented the other one from drifting?
