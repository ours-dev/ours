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

## Every zone gets three watermarks, scaled out of a machine-wide reserve (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `WaterMark` becomes `Min`/`Low`/`High`; adds `watermark()`, `watermark_ok()` and a private `setup_watermarks()`
- `kernel/mm/pm_zone.cpp` — computes the three numbers for one zone out of its share of the reserve
- `kernel/mm/include/ours/mm/pm_node.hpp`, `kernel/mm/pm_node_init.cpp` — `PmNode::setup_watermarks()` sums every zone's managed total, turns it into the machine-wide reserve, and hands each zone its share; `isqrt()` and `machine_reserve_frames()` are local to the file
- `kernel/mm/init_pmm.cpp` — calls it once, after the handoff

**Symptom**
None. `m_watermarks` had no writer and `PmZone::alloc_frames()`'s gate reads `is_below_high_wmark()`, which nothing sets, so no allocation has ever been measured against anything. That stays true after this change: it computes the numbers and nothing yet consults them.

**Cause**
Not a defect — the first half of giving the allocator a notion of "running short", split from the half that acts on it.

Why three numbers rather than one. A single "low on memory" line cannot serve the two different questions the allocator has to answer. The first is whether to admit *this* allocation, and the answer differs by caller: one that can block should be turned away early and sent to the next zone, because it has somewhere else to go and because what is left here may be the only memory a caller that cannot block could ever use. That gives two admission lines, `Low` and `Min`. The second question is whether the zone is worth reclaiming, and it has to be answered *above* both admission lines — a reclaimer woken only once allocations were already being refused would always be too late. That is `High`, and it is deliberately not an admission line at all.

Why they are per zone rather than per node, which was the open question when this started. Zones exist here to serve callers with addressing constraints: a driver that can only reach the low 16 MiB can be out of memory while the Normal zone holds hundreds of megabytes, and a node-wide free count is blind to exactly that. No such caller exists in the tree yet, which was briefly mistaken for a reason to put the numbers on the node; it is not one, because the absence is a gap in the project rather than a property of the design, and the wake-up signal a reclaimer will need carries the zone that tripped anyway (Linux's `wakeup_kswapd()` records `kswapd_highest_zoneidx`). Per-node would have had to be unpicked.

Why the floor grows with the square root of memory. The amount that has to stay free for the kernel to allocate its way out of trouble is roughly a function of how many things can be in flight at once, not of how much memory the machine has. A fixed fraction gets both ends wrong: 1% of 512 MiB is about right, 1% of a terabyte is ten gigabytes held back for nothing. The square root grows, but far more slowly than the memory, and the two clamps keep the small and large ends usable.

**Change**
The enum says what the numbers are for:

```cpp
    enum class WaterMark {
        Min,
        Low,
        High,
        Count,
    };
```

The machine-wide reserve, four times the square root of memory in KiB, clamped:

```cpp
    auto machine_reserve_frames(usize const managed_total) -> usize {
        constexpr auto KIB_PER_FRAME = usize(PAGE_SIZE / 1024);
        constexpr auto MIN_RESERVE_KIB = usize(128);
        constexpr auto MAX_RESERVE_KIB = usize(256 * 1024);

        auto const managed_kib = managed_total * KIB_PER_FRAME;
        auto const reserve_kib =
            std::clamp(isqrt(managed_kib * 16), MIN_RESERVE_KIB, MAX_RESERVE_KIB);
        return reserve_kib / KIB_PER_FRAME;
    }
```

and one zone's share of it:

```cpp
    auto const min = reserve_frames * managed / managed_total;
    auto const delta = std::max(min >> 2, managed * WATERMARK_SCALE / WATERMARK_SCALE_UNIT);

    m_watermarks[static_cast<usize>(WaterMark::Min)] = min;
    m_watermarks[static_cast<usize>(WaterMark::Low)] = min + delta;
    m_watermarks[static_cast<usize>(WaterMark::High)] = min + delta + delta;
```

`min` is proportional rather than equal across zones, so the floor costs every zone the same fraction of itself: a 16 MiB Dma zone and a 500 MiB Normal zone cannot hold back the same number of frames. `delta` is a quarter of the zone's own floor or `WATERMARK_SCALE` of the zone, whichever is larger; on a large machine a quarter of a square-root-sized floor is too narrow a band for a reclaimer woken at `High` to finish inside before allocations start being refused at `Low`.

The query side, which 2b will consult:

```cpp
    auto watermark_ok(usize const order, WaterMark const mark) const -> bool {
        auto const free = free_frames();
        auto const wanted = usize{1} << order;
        if (free < wanted) {
            return false;
        }
        return free - wanted >= watermark(mark);
    }
```

Deliberately different from the reference here. Linux's `__zone_watermark_ok()` compares `free_pages` against the mark without subtracting the size of the request, and checks separately that a block of the requested order exists. Subtracting it matters more here: `MAX_FRAME_ORDER` frames is a large fraction of a small zone's watermark, so a single high-order allocation could step straight through the floor while `free > min` still read true. The block-exists question is left to the buddy, which answers it by failing.

Rejected: putting the numbers behind a tunable. Linux's `min_free_kbytes` and `watermark_scale_factor` are sysctls because a workload can know better than the formula; there is no way to set one here yet and no workload to set it for, so they are constants with the reference's defaults and the formula written out where a tunable would later multiply in.

Rejected: a stored managed total per zone to divide by. See the previous entry in this file — it is computed from present minus reserved, and this is its first caller.

**Reference**
Linux, closely. `__setup_per_zone_wmarks()` in `mm/page_alloc.c` for the proportional share and the `min`/`low`/`high` ladder, `calculate_min_free_kbytes()` for `int_sqrt(lowmem_kbytes * 16)` clamped to `[128, 262144]`, `watermark_scale_factor = 10` for `WATERMARK_SCALE` out of 10000, and `int_sqrt()` in `lib/math/int_sqrt.c` for the bit-at-a-time square root. `__zone_watermark_ok()` is the model for `watermark_ok()`, with the one deviation noted above.

Not ported: Linux caps `WMARK_MIN` to a small value for highmem and movable zones, because `__GFP_HIGH` and `PF_MEMALLOC` allocations cannot use them. There is no highmem here, and `ZoneType::Movable` and `ZoneType::Dynamic` come out empty on every machine this boots on, so the cap has nothing to apply to. Also not ported: `lowmem_reserve[]`, which `__zone_watermark_ok()` adds to the mark so that an allocation permitted to use a low zone still cannot take the last of it. That is a second mechanism on top of the ordering, and the ordering itself is wrong here first (`docs/repairs/261028/mm.md` item 1).

Zircon has no counterpart: no zones, and one `free_count_` per node against which `PmmNode` publishes a single level.

**Verification**
`python3 scripts/format_cpp.py --check` clean on all five files. `prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86` on `pm_zone.cpp`, `pm_node_init.cpp` and `init_pmm.cpp` reports nothing on any changed line; what it does report sits on `pm_node.hpp`'s accessors and `pm_zone.hpp:25`, neither touched here.

`python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lane.memmap`:

```text
build      ok (out/lane.memmap, 1.7s)
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  148 passed, 0 failed
```

Identical counts to the run before, which is the claim: the numbers exist and nothing reads them.

`isqrt()` was checked against a reference by transcribing it and running it over 209282 values — every integer below 200000, every perfect square below 3000 squared with its two neighbours, and every power of two from 1 to 2^63 with its two neighbours. Zero mismatches. Worth doing separately because it is the one piece whose correctness reading does not settle, and because it is in an anonymous namespace where no test can reach it.

The same transcription gives this machine's numbers: the boot log reports `present frames: 129451` and `reserved frames: 3094` for node 0, so `managed_frames()` is 126357 — consistent with the handoff's own `Released 13 regions, 126322 frames (493 MiB)`, the 35-frame difference being frames in holes. That makes the machine-wide reserve 710 frames, 2843 KiB.

Not verified: the per-zone split of those 710 frames. `PmZone::setup_watermarks()` logs each zone's three numbers, but a kernel `log::info` goes to the log ring and only reaches the console when a panic replays it, so on a passing boot the line exists and is invisible. The arithmetic between the reserve and a zone's three numbers is therefore verified by reading, not by observation.

**Deliberately not done**
The gate. `PmZone::alloc_frames()` still refuses at `High`, which is not what a high watermark is for, and still reads `is_below_high_wmark()`, which nothing sets. Changing it is the next commit, on its own, so that if a boot breaks the cause can only be that.

With it: making `m_flags` safe to write from the allocation path. It is a plain `Flags` today, and `BelowHighWmark` is the one bit that would be set and cleared by allocating CPUs; a lost update there would not merely stale the hint, it could clobber `Initialized` and `Contiguous` in the same word.

With it: `gaf_to_allocation_control()`, which still returns an empty option set, so `AllocationOptions::NoBlock` is never set and the gate cannot yet tell a caller that can block from one that cannot.

No recomputation. `unwire_frame()` gives frames back later — the `.init` sections and the boot image's pages — so the managed total these were scaled by is an under-estimate of the final one, which makes every watermark slightly lower than it will eventually want to be. Linux re-runs `setup_per_zone_wmarks()` on hotplug; here the first thing with a reason to care is the reclaimer, so recomputation belongs with it.

No test that executes the per-zone arithmetic. A host test would have to compile `pm_zone.cpp`, which reaches `CpuLocal`, locks and logz4, so it is not the cheap kind the other two mm smoke tests are; a kshell test could assert the invariants on the real zones instead, and that is the better shape once there is something to assert beyond ordering.

**How it was found**
A feature, so nothing to find — but the attempt to make the numbers observable is worth recording, because it cost a broken boot and turned up a trap.

`PmNode::dump()` prints a block per zone and had never had a caller, so the plan was to add the watermarks to it and call it from `init_pmm()`. Both images then panicked with `[... error] operator new`, immediately after the node's own block printed and before any zone's. The message names the kernel's plain `operator new`, which `kernel/lib/libc/cpp_dependencies.cpp:16` defines as `ours::panic(__func__)` and which nothing ever defines strongly — so an allocation from a log line is always fatal, at any stage, heap or no heap.

The question that mattered was whether `dump()` had always been a latent panic or whether the four added values caused it, because only one of those is someone else's bug. One experiment settled it: reverting the four values and keeping the call made both images boot. So it was the length. logz4 formats a line's body with `vformat_to` into a bounded output iterator (`system/lib/logz4/src/log.cpp:199`), libc++ cannot write through a custom iterator directly and buffers into its own 256-byte array (`prebuilt/clang/llvm-23/include/c++/v1/__format/buffer.h:461`), and a body that does not fit retargets to an allocating buffer (`:579-580`). Counting characters agreed: the original per-zone block formats to roughly 200 bytes and the four extra values took it to roughly 310.

Filed as `docs/repairs/261028/logz4.md` item 1, because a silent 256-byte limit on a log line whose violation panics naming `operator new` will be hit again by someone with no reason to suspect the formatter.

**The user's prediction**
None — homework is paused. Two decisions were theirs: that reclaim becomes its own activity above the page queues rather than a step inside a failed allocation, and, after pushing back on the claim that a per-zone shortage has no victim, that the watermarks are Linux's `min`/`low`/`high` rather than the `Critical`/`Moderate`/`Sufficient` pressure bands the enum used to carry.

**Three questions you should be able to answer**
1. `High` is not an admission line. Say what it is for, and why a reclaimer woken at `Low` instead would always be too late.
2. `min` is proportional to each zone's managed total rather than split equally. What happens to a 16 MiB Dma zone under an equal split, and what happens to it under the lower clamp of the machine-wide reserve on a very small machine?
3. `watermark_ok()` subtracts the size of the request before comparing, and Linux does not. Give the case where that difference decides the answer, and say why it bites a small zone harder.
