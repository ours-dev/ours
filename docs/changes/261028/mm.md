# mm changes

## PmZone's free counter stops calling itself the managed total, and the total becomes real (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `m_managed_frames` renamed `m_free_frames`; `free_frames()` is now the cheap atomic read; `managed_frames()` is computed from present minus reserved; the locked walk becomes `count_free_frames()`
- `kernel/mm/pm_zone.cpp` — the counter's two writers follow the rename; the constructor's initializer list gains the three members it had been skipping
- `kernel/mm/pmm.cpp` — `free_frame_count()` calls `count_free_frames()`

**Symptom**
Latent, cannot be triggered today: nothing read `managed_frames()`. `PmNode::dump()` prints `present_frames()` and `reserved_frames()` and stops there, so the one accessor that answered the wrong question had no caller to mislead. It would have become wrong output the moment anything printed it, and wrong arithmetic the moment anything scaled by it — which is exactly what per-zone watermarks need to do.

**Cause**
One counter was wearing two jobs' names. The member was called `m_managed_frames` and carried this comment:

```cpp
//! @brief present pages managed by the buddy system, which
//! is calculated as (reserved_pages includes pages
//! allocated by the booting time allocator):
//!     managed_frames_ = present_frames_ - reserved_frames_.
```

That describes a constant: how much memory the buddy owns in this zone, fixed once the boot allocator has handed over. But the code treated it as a running free count — `finish_allocation()` subtracted the allocated block, `free_frame()` added the freed one. As a free count it is correct, and that is worth being precise about: it starts at zero, the handoff releases this zone's frames into the buddy and each release counts itself in, and a frame has to be freed in before it can be allocated out, so it cannot go negative. The mechanism was never broken. The name and the comment simply described a different quantity than the code maintained.

Why that matters now rather than as tidiness. Watermarks are two different numbers meeting: they are **computed from the total** a zone manages (Linux scales each zone's share by `zone_managed_pages(zone)`) and **compared against the free count** (`zone_page_state(zone, NR_FREE_PAGES)`). With one counter answering to both names there was no total to scale by at all, and the next person to write `managed_frames() - free_frames()` would have got a number with no meaning. A name that lies is worse than a missing accessor, because the missing one cannot be used by mistake.

There was a second thing hiding behind it. The zone had no cheap free count that the allocation path could consult: `PmZone::free_frames()` took the zone lock and asked the buddy to walk its free lists. A watermark check wants to run *before* the lock, so that a zone known to be short can be skipped without paying for it; calling the locked version there would take the lock, drop it, and have `alloc_frame()` take it again one line later for an answer that was already stale. The cheap counter existed the whole time — it was the atomic, under the wrong name.

Finally, the constructor's initializer list opens with "Initializer order follows the member declaration order in pm-zone.hpp" and then skipped `m_managed_frames`, `m_reserved_frames` and `m_watermarks`. The values came out right anyway, because `std::atomic` and `std::array` value-initialise themselves, so this was never a bug — but a list that claims to be exhaustive and is not stops being readable as a check on the declaration order, which is the only reason to write one in full.

**Change**
The member says what it holds, and the two writers follow:

```cpp
    std::atomic<usize> m_free_frames;
```

```cpp
auto PmZone::finish_allocation(PmFrame *frame, Gaf gaf, usize order) -> void {
    m_free_frames -= usize{1} << order;
}
```

`free_frames()` becomes the cheap read, documented as approximate on purpose:

```cpp
    FORCE_INLINE
    constexpr auto free_frames() const -> usize {
        return m_free_frames;
    }
```

and the total becomes what its comment always claimed:

```cpp
    FORCE_INLINE
    constexpr auto managed_frames() const -> usize {
        return m_present_frames - m_reserved_frames;
    }
```

The locked walk keeps its behaviour and takes a name that says it walks, `count_free_frames()`, matching the vocabulary `pm_zone_test.cpp` already uses for its own `count_free_blocks()` helper. Its one caller, `free_frame_count()` in `pmm.cpp`, follows; it is a diagnostic that sums every zone, so it keeps the exact answer rather than the cheap one.

Rejected: storing the managed total in a third atomic, as Linux does with `zone->managed_pages`. Linux stores it because it is read on allocation paths and under hotplug; here it is read once, at watermark setup, and never on a hot path. A stored copy would be a third counter to keep in step with the two it is derived from, and the only way it could ever disagree with them is by someone forgetting to update it.

Rejected: giving the cheap counter a hedged name like `free_frames_approx()` and leaving `free_frames()` on the locked walk. It puts the expensive, exact answer behind the obvious name, which is the wrong default for a counter that the allocation path is meant to consult; the approximation is the normal case and the walk is the special one.

**Reference**
Linux keeps the two apart by construction: `zone_managed_pages(zone)` is the total (`include/linux/mmzone.h`), the free count is a vmstat counter read with `zone_page_state(zone, NR_FREE_PAGES)`, and `__setup_per_zone_wmarks()` in `mm/page_alloc.c` uses the first to compute what `__zone_watermark_ok()` compares the second against. The vmstat counter is per-CPU-batched and therefore approximate, which is the same trade this change writes down in `free_frames()`'s comment. Zircon has no zones and so no counterpart; its `PmmNode` keeps one `free_count_` for the whole node.

**Verification**
`python3 scripts/format_cpp.py --check` clean on all three files. `prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86` on `pm_zone.cpp` and `pmm.cpp` reports nothing on the changed lines; the one diagnostic it gives, `lists_` at `pm_zone.hpp:25`, is on a line this change did not touch. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lane.memmap`:

```text
build      ok (out/lane.memmap, 30.4s)
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  148 passed, 0 failed
```

Identical counts to the run before the change, which is the whole claim: this is a rename plus one computed accessor, and nothing should have moved.

Not measured: `managed_frames()`'s value. It has no caller yet — the watermark setup that will be its first one is the next change — so the arithmetic is verified by reading the two counters it subtracts, not by seeing a number printed.

**Deliberately not done**
Watermarks. This change only makes the two quantities they need distinguishable; it computes nothing and changes no allocation decision. `m_watermarks` still has no writer and `PmZone::alloc_frames()` still gates on `is_below_high_wmark()`, which nothing sets.

The gate itself. It refuses an allocation at the **high** watermark, which is not what a high watermark is for — Linux allocates freely above it and only uses it to decide that a zone is worth reclaiming, refusing at min or low instead. Correcting it needs the watermarks to exist first, so it belongs to the next change rather than this one.

`m_frames`, a `std::array<PmFrame::List, NR_FRAME_ORDERS>`, is still left out of the initializer list and still appears to have no reader now that the free lists live in `m_fset`. Not touched: an unused member is often scaffolding, and establishing that this one is not takes a separate look.

**How it was found**
Not by a scan. Deciding where per-zone watermarks go meant reading Linux's `__setup_per_zone_wmarks()` to see what they are computed from, which named two quantities — `zone_managed_pages()` and `NR_FREE_PAGES` — and then looking for both here. `grep -rn m_managed_frames` returned four lines: the declaration, the accessor, and one `-=` and one `+=`. A counter that is decremented when memory is handed out cannot be a constant total, so the comment above it had to be describing something else. `grep -rn "managed_frames()"` then showed no caller anywhere, which is why nothing had noticed.

**The user's prediction**
None — homework is paused.

**Three questions you should be able to answer**
1. Watermarks need a total and a free count. Say which of the two each of `managed_frames()` and `free_frames()` is, and what goes wrong if one counter is asked to be both.
2. `free_frames()` is documented as approximate and `count_free_frames()` as exact. Why does the allocation path want the approximate one, and what would taking the exact one there cost beyond time?
3. The old counter was never numerically wrong. What was the actual defect, and why was "nothing reads it" the reason it survived rather than a reason to leave it alone?
