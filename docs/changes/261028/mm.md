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

## The physical allocator loses the state nothing reads (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `FrameSet` deleted; `PmZone` holds a `FrameBuddy` directly; `m_frames` deleted
- `kernel/mm/pm_zone.cpp` — the six `m_fset` call sites become buddy calls; `FrameSet`'s two loops land in the functions that use them
- `kernel/mm/include/ours/mm/pm_node.hpp` — `m_present_frames`, `m_reserved_frames` and the two reserved-frame accessors deleted
- `kernel/mm/pm_node_init.cpp` — the present-frame accumulator goes with them
- `kernel/mm/pm_node.cpp` — `dump()`'s node half drops the two counts
- `kernel/mm/memmap_init.cpp`, `kernel/mm/pmm.cpp` — the node-side reserved-frame calls go; the zone-side ones stay

**Symptom**
None. Nothing read any of it, which is the point.

**Cause**
Not a defect; a depth problem, and the first step of `.agents/tasks/pmm-layering/DESIGN.md`. Four pieces of state, each judged by the same question: delete it, and where does the complexity reappear?

`FrameSet` was a wrapper over `FrameBuddy` with five methods, three of them one-line forwards, and exactly one caller. Delete it and nothing reappears: `acquire_frame`, `release_frame` and `free_blocks` are the buddy's own calls under another name, and the two that do contain logic — the weighted free-frame walk and the "is any order at or above this one non-empty" test — have one call site each and belong there. An interface nearly as complex as what sits behind it is the definition of shallow.

`PmZone::m_frames`, a `std::array<PmFrame::List, NR_FRAME_ORDERS>`, had no reader at all. It is the shape of the buddy's free lists, kept beside the buddy's free lists. The risk was never that it was read; it was that it would be written, by someone who found a per-order list array on the zone and reasonably assumed it was the zone's.

`PmNode::m_present_frames` and `m_reserved_frames` were the sum of their zones' own counters, maintained in parallel with them — `memmap_init.cpp` incremented both the node's and the zone's one frame at a time, and `unwire_frame()` decremented both. Two counters for one fact, and the only reader of the node's pair was `PmNode::dump()`, which has no caller. Delete them and the information is still there, once, where the frames are.

What makes this worth a record rather than a tidy-up is the fourth piece, because the first reading of it was wrong. The design claimed `PmNode::m_spanned_frames` was in the same position — read only by the uncalled `dump()`. It is not. `pm_node_init.cpp` reads it one line after writing it:

```cpp
m_spanned_frames = node_end_pfn - node_start_pfn;

if (m_spanned_frames != 0) {
    set_state(m_id, State::Memory);
}
```

`State::Memory` is what `Router::build()` walks to chain a node's zones behind another's, so a node wrongly left out of it would get no remote fallback. The member is dead *after* `init()`; the value is not dead at all. "Nothing reads it" has to mean nothing, and a `grep` for the field name is how to find that out rather than how to confirm it.

**Change**
The zone holds the buddy, and the locking rule `FrameSet` documented moves with it rather than being deleted along with the class:

```cpp
    /// The zone's free lists.
    ///
    /// The buddy has no lock of its own, on purpose, and this zone is the only
    /// thing that reaches it: a second lock down there would serialise the
    /// same accesses twice and, worse, give the allocator two places to take a
    /// lock from, which is how a path that allocates and frees under one call
    /// ends up deadlocking against itself. So the rule is that PmZone
    /// serialises and the buddy does not, and every use of this member holds
    /// m_lock -- except the constructor's, which gives it the zone's span
    /// before the zone is on any list another CPU could reach it through.
    FrameBuddy m_buddy TA_GUARDED(m_lock);
```

`TA_GUARDED` moving from the wrapper to the buddy is what keeps the thread-safety analysis checking the same thing it checked before.

The two loops land where they are used, so that the cost of each is visible at its call site:

```cpp
auto PmZone::count_free_frames() -> usize {
    ZoneGuard guard{m_lock};
    usize total = 0;
    for (usize order = 0; order < FrameBuddy::NUM_ORDERS; ++order) {
        total += m_buddy.free_count(order) << order;
    }
    return total;
}
```

Rejected: keeping `FrameSet` and only deleting `m_frames`. The wrapper's stated reason for existing — that the buddy stays free of kernel globals so the host tests can run it — is real, but it is `FrameBuddy` that satisfies it, and `FrameBuddy` is already in `priv/frame_buddy.hpp` with its own host test. The wrapper adds nothing to that.

Rejected, and this narrows what the design proposed: making `m_spanned_frames` a local. It would leave `m_start_pfn` beside it describing a range with no length, and the two together are a real property of a node rather than a leftover. So both stay, and only the two counters that genuinely duplicated the zones' go.

**Reference**
Linux's `struct zone` holds `free_area[MAX_ORDER + 1]` directly; there is no object between the zone and its free lists. Zircon's `PmmArena` likewise holds its `vm_page_t` array and its free list without a wrapper. Neither keeps a node-level copy of a per-zone count: Linux's `pg_data_t` has `node_present_pages` and `node_spanned_pages` but they are set once at bring-up from the zones and are read by `/proc` and hotplug, not maintained in parallel on the allocation path the way these were.

The deletion test itself is not from either: it is the vocabulary of `.claude/skills/codebase-design`, where a module earns its keep if deleting it makes complexity reappear across its callers.

**Verification**
`python3 scripts/format_cpp.py --check` clean on all seven files. `prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86` on the five sources reports nothing on a changed line; what it does report — `nodiscard` on five `ZoneList` accessors and `lists_` inside `FramePcpuCache` — is on lines this change did not touch.

```text
build      ok (.worktrees/feat-pmm-layering/out/bringup.x86, 23.8s)
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  148 passed, 0 failed
```

Identical to the worktree's baseline, which is the whole claim of this step and of every step of this task: a count that moves means the step was not behaviour-neutral.

**Deliberately not done**
The per-CPU frame cache stays, although `MAX_PCPU_CACHE_ORDER` is 0, `is_order_within_pcpu_cache_limit()` is therefore always false, and both `with_current` lambdas are unreachable and empty. The deletion test does not settle it — it has no callers for complexity to reappear across — so what argues for keeping it is the rule about scaffolding for planned work, not depth. It is also the only thing in this cluster that would change the locking story, since a cache that serves order-0 allocations without the zone lock is the reason the zone lock's granularity matters, and making it real is a performance item.

`PmNode::m_start_pfn` and `m_spanned_frames` stay, for the reason above.

Nothing else from the design. `AllocRequest`, `ZoneSelector`, the registry, the topology and the admission gate are steps 2 through 8 of `.agents/tasks/pmm-layering/PLAN.md`, one commit each.

**How it was found**
Taking an inventory for the layering design, not by a scan for bugs. `grep -rn m_frames` returning one line — the declaration — is what flagged the first; `grep -rn m_managed_frames`'s four lines flagged the counter rename two commits earlier, and the same method applied to the node's counters found a pair whose only reader was a function with no caller. `FrameSet` came out of reading its own interface next to `FrameBuddy`'s and counting the forwards.

**The user's prediction**
None — homework is paused. D4 of the design was decided as recommended on 2026-10-28, with the `m_spanned_frames` correction above raised before the work rather than after.

**Three questions you should be able to answer**
1. `FrameSet` existed so that the buddy algorithm stayed free of kernel globals and could run in a host test. Why does deleting it not cost that property?
2. `TA_GUARDED` moved from `FrameSet m_fset` to `FrameBuddy m_buddy`. What would the thread-safety analysis have stopped checking if it had not?
3. The design said `m_spanned_frames` was read only by an uncalled `dump()`, and it was wrong. What is the reader, what would have broken, and what kind of grep would have caught it the first time?

## An allocation request becomes one immutable value (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` — new: `AllocRequest` and the pure `resolve_request()`
- `kernel/mm/include/ours/mm/priv/memory_allocation_context.hpp` — deleted
- `kernel/mm/include/ours/mm/gaf.hpp` — `AllocationOptions` deleted
- `kernel/mm/include/ours/mm/pm_node.hpp`, `kernel/mm/pm_node.cpp` — `alloc_frames_preamble()` and `gaf_to_allocation_control()` go; the walk and the postamble take the request
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `alloc_frames()` takes the request; its gate loses the term that never distinguished anything
- `kernel/mm/tests/alloc_request_smoke.cpp`, `kernel/mm/tests/BUILD.gn` — new host test, with NUMA forced on for it alone
- `BUILD.gn` — mm's host tests reach `//:default` through their group instead of one entry per executable

**Symptom**
None in the kernel; this is step 2 of `.agents/tasks/pmm-layering/PLAN.md` and changes no behaviour. The host-test count moves from 148 to 149, which is the only number this step is allowed to change.

**Cause**
One allocation was four values, and the one with the best shape was dead.

`Gaf` carried the caller's intent. `AllocationOptions`, with `gaf_to_allocation_control()`, was meant to be the policy derived from that intent and was threaded through three signatures while answering with the empty set for every `Gaf`. `MemoryAllocationContext` carried `{initiator, allowed_nodes, home_node}`, filled by a preamble whose return type was `Status` and whose only answer was `Ok`. And `AllocationContext`, with a `ZoneIterator` of its own, carried the request *and* its walk — but only `kernel/commands/pm_node_test.cpp` ever built one; the production path was rewritten onto `ZoneList::for_each_zone` with the node filter copied out by hand.

The cost was not the type count. It was that no layer could be handed "the request", so each was handed three parameters and re-derived from them what it needed, and anything genuinely derived from the caller's ask — which watermark admits it, which zone types it may use — had nowhere to live. That is why the admission gate designed before this had to be parked inside the object that owns the free lists.

Why the data fuses and the cursor splits, rather than either taste or symmetry: the deletion test, applied to the split. Separate the derived policy from the caller's ask and nothing reappears — both have the same lifetime, the same owner and the same immutability, so they are one value's worth of data. Separate the **cursor** and something real changes: a mutable iteration position stops living inside a value that is otherwise read-only. `AllocationContext` has that mutable position inside it today, and the first draft of this design was about to reproduce it.

**Change**
The value, immutable, built once:

```cpp
struct AllocRequest {
    Gaf gaf;
    usize order;
    NodeMask allowed_nodes;
    NnId home_node;
    CpuNum initiator;
};
```

and the resolver, pure so that a host test can reach it:

```cpp
inline auto resolve_request(Gaf const gaf, usize const order, NodeMask const &fallback,
                            NnId const home, CpuNum const initiator) -> AllocRequest;
```

It takes the calling CPU rather than reading it, and the home node rather than finding it, because which node serves an allocation is a different question answered before this one. The parameter lists collapse accordingly:

```text
before  get_frame_from_zone_list(Gaf, usize, AllocationOptions, MemoryAllocationContext const &)
after   get_frame_from_zone_list(AllocRequest const &)

before  PmZone::alloc_frames(Gaf, usize, AllocationOptions, MemoryAllocationContext const &)
after   PmZone::alloc_frames(AllocRequest const &)

before  alloc_frames_preamble(Gaf, usize, NodeMask const &, MemoryAllocationContext *) -> Status
after   (gone: resolve_request returns the value instead of filling one and answering Ok)
```

Rejected, and this narrows what the design said: folding `apply_thread_policy()` into `resolve_request()`. The design claimed the resolver absorbs it. Reading it first was worth more than carrying that out — the function is entirely a stub, a `switch` over four policy kinds whose every case is an empty `break`, and it takes `Gaf &` without ever writing it. Folding a stub in would move it, not resolve anything, and it answers the different question of *which node* rather than *what this request is*. It stays where it is until it does something.

Rejected: keeping `AllocationOptions` as the place the admission policy would later live. That was decision D8 of `.agents/tasks/flag-consolidation/DESIGN.md`, taken on 2026-10-04, which kept it and `gaf_to_allocation_control()` on exactly those grounds. D1(c) of this task's design, taken 2026-10-28, moves that place to the request and the selector, so the reason for keeping it is gone — this supersedes D8 rather than overlooking it.

**Reference**
Linux's `struct alloc_context` in `mm/internal.h`: one value built once per allocation, carrying the zonelist, the nodemask, the preferred zone reference, the highest zone index and the migratetype, passed by pointer to `get_page_from_freelist()` and to the slow path. `prepare_alloc_pages()` is `resolve_request()`'s counterpart, and `gfp_to_alloc_flags()` is the mapping that `gaf_to_allocation_control()` was a stub of. The one deliberate difference: Linux's `ac` also holds `preferred_zoneref`, a cursor into the zonelist, which is the part kept out here.

Zircon has no counterpart — `PmmNode::AllocPages()` takes an `alloc_flags` word and nothing else, because with no zones and one node per machine there is nothing to resolve.

**Verification**
`format_cpp.py --check` and `format_gn.py` clean on every changed file. `clang-tidy -p out/bringup.x86` reports nothing on a changed line in `pm_node.cpp`, `pm_zone.cpp` or the new test; the `nodiscard` and `lists_` warnings it does give are on lines untouched here.

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed
```

The kernel counts are identical to step 1; the host count is one higher, which is the new test. Its ten checks pin what was spread across the preamble: the caller's `gaf`, order, home node and initiator reach the request unchanged; without `ThisNode` the whole mask is allowed; with it the home node is kept and every other dropped; a mask that excludes the home node neither gains it nor empties; and an empty mask stays empty.

**Deliberately not done**
The request carries no zone-type range and no admission mark. Both have readers only at later steps — the range at step 3, where the selector reads both of `ZoneIterator`'s bounds, and the mark at step 8 — and a field that arrives before its reader is the pattern this whole task exists to undo.

`PmZone::alloc_frames()` still gates on `is_below_high_wmark()`, which still has no writer, and still tests the wrong line. Step 8.

The node filter is still written out in `get_frame_from_zone_list()` as well as in `ZoneIterator::move_next()`. Step 3 deletes one of them.

**How it was found**
A feature, but two traps are worth the record because both would have shipped quietly.

The new test compiled, linked, and did not run. `//kernel/mm/tests:tests` was not reachable from `//:default`: the root `BUILD.gn` named `frame-buddy-smoke` and `protection-ranges-smoke` individually, under a comment explaining that the group had sat unwired. `scripts/host_tests.py` finds exactly the testonly host executables that `//:default` reaches, so a test added to the group alone is invisible to it. The symptom was the host count staying at 148 while `gn ls` claimed not to know the target at all — which is consistent, because GN does not generate a target nothing reaches. The fix was to wire the group and delete the two individual entries, so there is one place to add the next one.

Then the test failed three checks, and the three were exactly the ones expecting a bit to be *set*. `NUMA_MAX_NODE` is 1 in the default build, and the test used node ids 1 and 2 against a one-bit mask, so every `set()` was out of range and every `test()` answered false. The deeper point is the one that made it worth fixing rather than renumbering: with one node the `Gaf::ThisNode` narrowing cannot be observed at all, because the caller's mask, the home node and the result are all `{0}`. So the target defines `OURS_CONFIG_NUMA=1` for this test alone — the function's logic does not depend on the configured node count, and 32 nodes is where its branches exist.

**The user's prediction**
None — homework is paused. D1(c) and D2(a) were decided on 2026-10-28, and D1(c) is what retires `AllocationOptions`.

**Three questions you should be able to answer**
1. The derived policy fused into the request but the cursor did not. State the deletion-test reasoning for each half.
2. `resolve_request()` takes the calling CPU and the home node as arguments instead of reading them. Name both things that buys.
3. A host test can be compiled, linked and never run. What does `scripts/host_tests.py` actually look for, and which single edit made the new test visible to it?

## Choosing a zone becomes an object, and both of its type bounds get read (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/priv/zone_selector.hpp` — new: `ZoneSelector`, holding the cursor and the three filters
- `kernel/mm/include/ours/mm/priv/zone_iterator.hpp` — deleted, with `AllocationContext`
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` — the request gains `lowest_zone` and `highest_zone`
- `kernel/mm/pm_node.cpp` — `get_frame_from_zone_list()` drives the selector; the hand-copied node filter goes
- `kernel/commands/pm_node_test.cpp` — five cases move from the dead walk to the live one, and one of them could not have been written before

**Symptom**
None observable. The zone-type range is read where nothing read it, and no zone of a type above `Normal` is ever created on any machine this boots on, so the new filter excludes nothing today.

**Cause**
Step 3 of `.agents/tasks/pmm-layering/PLAN.md`, and the step the whole task was really about.

Choosing which zone serves an allocation lived in a lambda inside `PmNode::get_frame_from_zone_list()`. That is not a small thing in the wrong place; it is a responsibility with no object, and three consequences followed from its absence.

The node filter was written twice. Once in that lambda, and once in `ZoneIterator::move_next()` — a walk that nothing on the allocation path used, kept alive only by `pmnode.test`. The test's own comment said so: "Rewritten with ZoneList and not on the allocation path yet, so nothing else exercises them."

The zone-type lower bound was stored and never read. `ZoneIterator` had `lower_zone_type` and `upper_zone_type`, `set_zone_range()` wrote both, and `move_next()` tested only the upper one, under the comment `// Reserved and not used now.` A caller asking to be kept off the low zones would have been served from them.

And `Gaf::ThisNode` had two mechanisms for one intent. `resolve_request()` narrows the request's node mask to the home node, which the live walk's node filter then enforces. `AllocationContext::build()` instead switched the candidate list, from the fallback order to the node's own zones by type. Both reach the same result, and only one of them ran.

The deeper consequence is the one that produced a rejected design two days of work ago: with no selector, a question about whether a candidate zone *qualifies* had nowhere to live, so an admission check against a zone's watermarks was put inside `PmZone` — the object that owns the free lists — making the zone at once the policy and the mechanism. Naming the selector is what gives that check a home, and it is why this step comes before the gate rather than after.

**Change**
The selector holds the cursor and nothing else, which is the reason it is a separate object from the request rather than a field of it:

```cpp
class ZoneSelector {
  public:
    ZoneSelector(ZoneList::Zones const zones, AllocRequest const &request)
        : m_zones(zones), m_request(&request), m_cursor(0) {}

    auto next() -> PmZone * {
        while (m_cursor < m_zones.size()) {
            auto *const zone = m_zones[m_cursor++];
            if (!zone) {
                continue;
            }
            auto const type = zone->zone_type();
            if (type < m_request->lowest_zone || type > m_request->highest_zone) {
                continue;
            }
            if (!m_request->allowed_nodes.test(zone->which_node())) {
                continue;
            }
            return zone;
        }
        return nullptr;
    }
    ...
};
```

and the walk becomes a loop over it, with no filter of its own:

```cpp
auto PmNode::get_frame_from_zone_list(AllocRequest const &request) -> PmFrame * {
    ZoneSelector selector{m_zone_list.fallback(), request};
    while (auto *const zone = selector.next()) {
        if (auto *const frame = zone->alloc_frames(request)) {
            return frame;
        }
    }
    return nullptr;
}
```

The candidate list is the fallback order in every case, because `Gaf::ThisNode` is served by the mask `resolve_request()` has already narrowed. One mechanism, and the one that was running.

The request carries the range:

```cpp
    ZoneType lowest_zone;
    ZoneType highest_zone;
```

set once, to `Dma` through `Normal`, in `resolve_request()`. Nothing new is expressible yet — no `Gaf` bit names a zone — but the all-zones default is now a value in one place rather than a constant pair inside the walk, which is the difference between a default and a law. `Movable` and `Dynamic` sit above the bound deliberately: a caller that has not said it can take movable memory must not be given it.

Rejected: keeping `ZoneIterator`'s setter chain (`set_nodes().set_zone_range().set_zones()`). Every one of those is now a field of the request, decided before the walk begins; a builder for a value that is already built is a second way to say the same thing, and it was what let the lower bound be written and ignored.

Rejected: making the selector a range with `begin()`/`end()` so the walk could be a range-for. It reads well for a traversal, and this is not one — the loop stops at the first zone that yields a frame, and `next()` returning null says "no more candidates" where an iterator would have to encode the same thing as equality with `end()`. `ZoneList::for_each_zone()` already provides the traversal shape for the callers that want it.

**Reference**
Linux's zonelist walk: `for_next_zone_zonelist_nodemask()` in `include/linux/mmzone.h`, which advances past zones above `highest_zoneidx` and outside the nodemask, driven from `get_page_from_freelist()` in `mm/page_alloc.c`. The cursor there is `struct zoneref *z` held by the caller's loop, and `ac->preferred_zoneref` is where it starts — so Linux keeps the cursor next to the walk and not inside `alloc_context` either, except for that starting point.

Linux has no lower bound to forget, because its zonelist is built from the highest zone down and a walk needs no floor. Ours is built from the lowest up (`docs/repairs/261028/mm.md` item 1), which is why a floor exists here at all and why forgetting to read it mattered.

Zircon has no counterpart: no zones, so no candidates to select among.

**Verification**
`format_cpp.py --check` clean on every changed file. `ours-verify`:

```text
build      ok (.worktrees/feat-pmm-layering/out/bringup.x86, 22.4s)
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed
```

Identical to step 2, as every step of this task must be.

That is not the verification that matters here, though, and the distinction cost a check. `boot_check.py` does **not** run the kshell `*.test` commands: it counts the `checks passed` lines that `system/app/shell/shell_tests.cpp` prints from userspace, and a kernel `log::info` from a kshell test goes to the log ring, which only reaches the console when a panic replays it. So an unchanged `44 passed` says nothing about `pmnode.test`. The tool for that is `scripts/kshell_run.py`:

```text
python3 scripts/kshell_run.py --out out/bringup.x86 pmnode.test pmzone.test

pmnode.test: pass  ZoneSelector steps over the holes in the by-type array
pmnode.test: pass  ZoneSelector's node mask filters out the excluded node
pmnode.test: pass  ZoneSelector's upper bound filters out higher zone types
pmnode.test: pass  ZoneSelector's lower bound keeps a caller off the zones below it
pmnode.test:   ThisNode selected 2 of 2 zone(s) in the fallback order
pmnode.test: pass  Gaf::ThisNode narrows the request's node mask, and the selector honours it
pmnode.test: ok -- 21 checks
pmzone.test: ok -- 3 checks
```

The fourth of those is the case that could not have been written against the code this replaces: a floor of `Dma32` must keep a caller off the `Dma` zones, and the bound that would have enforced it was never read.

Weak on this machine: the `ThisNode` case selected 2 of 2, because the dev guest has one node, so "this node's zones" and "the fallback order" are the same list. It checks that the mask is narrowed and honoured, not that anything is excluded. A NUMA configuration is what would make it bite.

**Deliberately not done**
The admission gate. `PmZone::alloc_frames()` still gates on `is_below_high_wmark()`, still has no writer for that flag, and still tests the wrong line. It now has a home to move to, which was the point of this step; moving it is step 8.

`ZoneList::for_each_zone_if()` has no callers at all — a one-line template over `for_each_zone()`. Left alone rather than deleted: it is a generic helper rather than a stub, and widening this step to judge it would blur what the step claims.

The zone walk's order. The selector walks the fallback order as it finds it, which is ascending by type, so every allocation still starts from the 16 MiB `Dma` zone (`docs/repairs/261028/mm.md` item 1). Fixing it is now a change in one place, which it was not before.

**How it was found**
A planned step, so nothing to find — but two things turned up while doing it, and one of them is about verification craft.

The first: the claim "identical boot counts" was about to be offered as evidence that the rewritten test cases pass. They were never run. `boot_check.py`'s counts come from userspace shell tests, and the kshell commands it never types are where these cases live. Checking what `boot_check.py` actually greps for — `"checks passed"`, which only `system/app/shell/shell_tests.cpp` prints — is what caught it before the claim was made rather than after.

The second is unrelated to this step and is filed as item 3 of `docs/repairs/261028/mm.md`: `set_zone_bounds()` loops to `NR_ZONES_PER_NODE`, which is 5, over a span the platform sizes by the number of *enabled* zone types, which is 3. Indices 3 and 4 are read off the end of the caller's stack array. It is benign today only because the loop's own carried-forward `start_pfn` has already reached `max_pfn` by then, so whatever is read collapses to an empty zone.

**The user's prediction**
None — homework is paused. D7 of the design was taken as (a) on the stated recommendation, flagged before this step began.

**Three questions you should be able to answer**
1. The cursor is in the selector and the node mask is in the request. Say what each would cost if it were in the other.
2. `Gaf::ThisNode` had two mechanisms. Describe both, and say why narrowing the node mask is the one to keep.
3. `boot_check.py` reported `44 passed` before and after a change to `pmnode.test`. Explain why that number cannot move, and name the tool that can.

## `std::to_underlying` replaces every enum cast in mm, which means giving the enums a base (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/types.hpp` — `ZoneType` and `MigrateType` get `: u32`; the two `NR_*` constants converted
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `WaterMark : u32`; the four watermark index sites
- `kernel/mm/include/ours/mm/pm_node.hpp`, `kernel/mm/pm_node_init.cpp` — `PmNode::State : u32`; nine index sites and the zone-bounds clamp
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `Role : u32`, `Flags : u32`; three `bit_width` arguments
- `kernel/mm/include/ours/mm/priv/sparse_memmap.hpp` — `MemSection::State : u32`, `MemSection::Type : u32`; two `bit_width` arguments
- `kernel/mm/include/ours/mm/page_queues.hpp` — the two sites that already called `std::to_underlying` and then cast the result anyway
- `.agents/skills/language/modern-cpp-development/SKILL.md` — the rule, with the enum-base condition that makes it work

**Symptom**
None. Twenty-three mechanical sites; `-Wconversion` is on and the build is clean after the change, which is the whole check.

**Cause**
`static_cast<usize>(ZoneType::Normal)` names a type the enum already knows. That is the defect: the cast keeps compiling after the enum's base changes, and converts to whatever width and signedness the cast says rather than to the enum's own. `std::to_underlying(x)` cannot be wrong about it — it asks the type.

The reason this is not a one-line substitution is the condition it exposes. An `enum class` with no explicit base has `int` as its underlying type, so `std::to_underlying` on it yields a signed `int`, and two things in this codebase reject that where the cast had hidden it:

- Indexing an array converts `int` to `size_t`, which `-Wconversion` (`build/config/BUILD.gn:185`, alongside `-Wall -Wextra -Werror`) objects to.
- `std::bit_width` is constrained to `__unsigned_integer` and refuses `int` outright. `pm_frame.hpp` and `sparse_memmap.hpp` compute bitfield widths with it.

So the honest form of the rule is: give an `enum class` that is used as an index or a bit count an explicit unsigned base. `: u32` is the one to reach for, because it is the width the implicit `int` already had — no member or bitfield changes size.

What the mixed state looked like before is worth recording, because it is the shape the rule prevents. `page_queues.hpp` had already reached for `std::to_underlying` and then wrapped the result:

```cpp
return static_cast<usize>(std::to_underlying(type));
```

Both halves, which is worse than either: the function asks the type and the cast then overrides the answer. That is what happens when the enum's base is left implicit and the cast is used to paper over the consequence instead of fixing the enum.

**Change**
Each enum gains the base it needed:

```cpp
enum class ZoneType : u32 { Dma, Dma32, Normal, Movable, Dynamic, Count };
```

and every site reads the enum rather than asserting over it:

```cpp
// before
return m_watermarks[static_cast<usize>(mark)];
std::array<usize, static_cast<usize>(WaterMark::Count)> m_watermarks;
Field<Index::Role, Role, ustl::bit_width(static_cast<usize>(Role::Count))>

// after
return m_watermarks[std::to_underlying(mark)];
std::array<usize, std::to_underlying(WaterMark::Count)> m_watermarks;
Field<Index::Role, Role, ustl::bit_width(std::to_underlying(Role::Count))>
```

Rejected: `: usize` instead of `: u32`. It also works and removes any question about widening, but it doubles the size of every stored enumerator — `PmZone::m_type`, `PmFrame`'s role and flags — for nothing. `u32` matches what `int` occupied.

Rejected: keeping `static_cast<usize>(std::to_underlying(x))` where an index wants `usize`. It compiles and it is what `page_queues.hpp` had; it is also the thing the rule exists to stop, and with an unsigned base the inner call already answers in an unsigned type that widens silently.

**Reference**
`std::to_underlying` is C++23, `<utility>`, standardised from `std::underlying_type_t` + `static_cast` precisely because writing the cast by hand lets the two drift. No Linux or Zircon counterpart: Linux is C, and Zircon's enums are cast by hand in the same way this change moves away from.

**Verification**
`format_cpp.py --check` clean on all nine files. `ours-verify`:

```text
build      ok (.worktrees/feat-pmm-layering/out/bringup.x86, 30.2s)
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed
```

Unchanged from step 3. The build being clean under `-Wconversion -Werror` is the substantive check: a missed enum base is a hard error, not a warning, which is how the one missed enum was found.

**Deliberately not done**
The rest of the tree, which is 211 further sites: 158 in `system/lib` (mostly `ustl`), 20 in `kernel/lib`, 9 in `kernel/kernel`, and a handful each elsewhere. Sweeping them is its own task, not a tail of this one, and `ustl` has a `.clang-format` of its own.

`kernel/mm/sparse_memmap.cpp:108` keeps a `static_cast<usize>(TOP_LEVEL)`. It is not an enum cast — `TOP_LEVEL` is already a `usize` constant — so it is a redundant cast rather than this rule's subject, and removing it would widen what this change claims.

**How it was found**
The user's rule, 2026-10-28, while reviewing the layering work: "不要再 static_cast<UnderlyingType>(Enum) 了，用 std::to_underlying".

The sweep then failed to build, and the failure is the useful part. `MemSection::Type` was the one enum left without a base, and the error cascaded into something that named neither the enum nor the cast:

```text
sparse_memmap.hpp:203:5: error: unknown type name 'Value'
```

`Value` is the `BitFields` alias three lines above; the real message — `no matching function for call to 'bit_width'`, `because 'int' does not satisfy '__unsigned_integer'` — only appeared when the file was compiled on its own with `-ferror-limit=30` and without `-Werror`, because the build's error limit had cut it off. Reading the first error of a cascade rather than the last is what `ninja`'s output does not do for you.

**The user's prediction**
None — homework is paused.

**Three questions you should be able to answer**
1. `static_cast<usize>(SomeEnum::Value)` compiles whatever the enum's base is. Name the two ways it can then be wrong.
2. Why does the rule need the enums to gain a base, rather than being a pure substitution? Name the two things in this build that reject a signed underlying type.
3. `page_queues.hpp` had `static_cast<usize>(std::to_underlying(type))`. Say what each half was doing and which one was the symptom.
## The machine's NUMA tables leave the page allocator's header (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/numa_topology.hpp` — new: `NumaTopology`, lifted verbatim out of `PmNode`
- `kernel/mm/numa_topology.cpp` — was `pm_node_topology.cpp`, renamed with the class
- `kernel/mm/include/ours/mm/pm_node.hpp` — the nested `Topology` class and its forward declaration go
- `kernel/mm/pm_node.cpp`, `kernel/mm/pm_node_init.cpp`, `kernel/mm/pmm.cpp`, `kernel/commands/pm_node_test.cpp`, `kernel/arch/x86/mm/srat.cpp` — 25 call sites renamed, and each gains the new include
- `kernel/include/ours/cpu.hpp` — a comment that named the old path
- `kernel/mm/BUILD.gn` — the renamed source, in the same position

**Symptom**
None. A verbatim lift plus a rename; the three tables, the eight accessors and every assertion in them are unchanged.

**Cause**
Step 5 of `.agents/tasks/pmm-layering/PLAN.md`. `PmNode::Topology` held three tables — a node-to-node distance matrix and two CPU-to-node arrays — and eight static accessors over them, and touched no instance state of `PmNode` at all. It was nested in the page allocator's node class while being neither a property of any one node nor anything the allocator owns: it is an input, filled by whatever can read the firmware, and read afterwards by the allocator, by a node's affinity masks and by anything that wants to know where a CPU sits.

The cost of the nesting was a dependency edge in the wrong direction. `kernel/arch/x86/mm/srat.cpp`, an arch file parsing ACPI tables, included `ours/mm/pm_node.hpp` — the page allocator's header, with `PmNode`'s zone list, page queues, allocation entry points and per-CPU accessor — in order to record that two nodes are a certain distance apart.

**Change**
The class moves out and is renamed for what it is:

```cpp
// kernel/mm/include/ours/mm/numa_topology.hpp
class NumaTopology {
  public:
    static auto distance(NnId x, NnId y) -> usize;
    static auto set_distance(NnId x, NnId y, usize distance) -> void;
    static auto cpu_to_node(CpuNum cpu) -> NnId;
    static auto current_node() -> NnId;
    static auto firmware_node(CpuNum cpu) -> NnId;
    static auto set_cpu_node_early(CpuNum cpu, NnId nid) -> void;
    static auto set_cpu_node(CpuNum cpu, NnId nid) -> void;
  private:
    static std::array<std::array<usize, NUMA_MAX_NODE>, NUMA_MAX_NODE> DISTANCE;
    static std::array<NnId, MAX_CPU> FIRMWARE_CPU_TO_NODE;
    static std::array<NnId, MAX_CPU> CPU_TO_NODE;
};
```

A class rather than free functions, because it has state and the state is private: the tables are not anyone's to index directly, and the accessors are where the `cpu < MAX_CPU` and `nid < NUMA_MAX_NODE` assertions live.

Rejected: free functions in a nested namespace, which is what Linux has (`cpu_to_node()`, `node_distance()` in `arch/x86/mm/numa.c`). It would mean either exposing the tables at file scope in the header or splitting the accessors from their storage, and the local idiom — `PmNode::Current`, `MemMap` — is a class holding its own tables.

Rejected: keeping a `using Topology = NumaTopology;` alias inside `PmNode` so the 25 call sites would not change. It would have made the move invisible, and the point of the move is that the name says where the thing belongs.

**Reference**
Linux keeps this out of `pg_data_t` entirely: `cpu_to_node()` and `numa_cpu_node` live in `arch/x86/mm/numa.c`, `node_distance()` in `arch/x86/mm/numa.c` over `numa_distance`, and `pg_data_t` in `include/linux/mmzone.h` holds zones, kswapd and the LRU. Zircon has no counterpart at all: CPU topology is not a VM concern there, and `Pmm` holds one node.

**Verification**
`format_cpp.py --check` clean on all nine files. `ours-verify`:

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed
```

and, because `boot_check.py` does not run them, `python3 scripts/kshell_run.py --out out/bringup.x86 pmnode.test pmzone.test`: `pmnode.test: ok -- 21 checks`, `pmzone.test: ok -- 3 checks`. `pmnode.test` is the one that reads these tables — `cpu_to_node`, `firmware_node`, `distance` and `current_node` all appear in its cases — so it is the check that matters here.

**Deliberately not done**
`srat.cpp` still includes `pm_node.hpp`. It needs exactly one thing from it: `PmNode::set_state(NnId{region.domain}, PmNode::State::Possible)`. That is the node registry, which is step 4, so the include edge this step was meant to cut only goes once both have landed. `PLAN.md`'s wording for step 5 claimed otherwise and is corrected in the same commit.

`kernel/mm/BUILD.gn`'s source list is not in gn's layout and `scripts/format_gn.py` reports it as unsafe to format — it was already so before this change (`frame_buddy.cpp` precedes `cpu_local.cpp`, and `page_request.cpp` is mis-indented). The renamed entry therefore stays in the exact position its predecessor held rather than being sorted into place, which keeps the link order identical; fixing the file's layout would reorder the sources, and the repo's rule is that reordering a GN list changes what the build produces.

**How it was found**
A planned step, but the build failure it produced is worth keeping, because it is the dependency becoming visible rather than a mistake.

`kernel/commands/pm_node_test.cpp` lives in namespace `ours`, not `ours::mm`, and had `using mm::PmNode;`. It then wrote `PmNode::Topology::current_node()` — and that resolved, because name lookup inside a class reaches its nested classes. Lifting `Topology` out broke six call sites with `use of undeclared identifier 'NumaTopology'; did you mean 'mm::NumaTopology'?`, fixed by one more using-declaration.

So the nesting had been handing an unrelated caller name lookup for free, which is exactly how a dependency stays unnoticed: the test never named `mm::` for the topology because it did not have to. The compile error is the edge that was always there finally being written down.

**The user's prediction**
None — homework is paused.

**Three questions you should be able to answer**
1. `NumaTopology` touched no instance state of `PmNode`. Why is "it compiles either way" not an argument for leaving it nested?
2. The test resolved `PmNode::Topology::` without ever writing `mm::`. Explain the lookup rule that allowed it, and what it concealed.
3. `srat.cpp` still includes `pm_node.hpp` after this change. For what, and which step removes it?

## `ZoneSelector` becomes a range, so the zone walk is a range-for (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/priv/zone_selector.hpp` — `next()` goes; the cursor moves into a nested `Iterator`, and the selector gains `begin()`/`end()`
- `kernel/mm/pm_node.cpp` — `get_frame_from_zone_list()`'s walk becomes `for (auto &zone: selector)`
- `kernel/commands/pm_node_test.cpp` — five walks become range-fors

**Symptom**
None observable. The same zones are visited in the same order; only the shape of the loop changes.

**Cause**
`ZoneSelector::next()` returned a raw `PmZone *` and used null as its sentinel, so the walk read:

```cpp
ZoneSelector selector{m_zone_list.fallback(), request};
while (auto *const zone = selector.next()) { ... }
```

That is a C iterator written in C++, and three things are wrong with it beyond the style.

It overloads one answer. Null means both "no candidate passed the filters from here on" and "there are no candidates", and a caller cannot tell the two apart — the shape forces the two facts into one value because a pointer has only one spare state. A range says the second thing by equality with `end()` and never has to say the first at all.

It makes the selector single-use, silently. The cursor was a member of the selector, so walking the same selector a second time continued where the first stopped and yielded nothing. A range hands out a fresh position per `begin()`, so a selector is what its name says — the candidates and the criterion — and the position belongs to whoever is walking.

And it does not read as what the code does. The candidate order plus a criterion *is* a range; `for (auto &zone: selector)` says "the zones this request may use" in one line, where `while (auto *const zone = selector.next())` says "keep calling a function until it gives up". There is a local precedent that already does this: `PmNode::Router` has `begin()`/`end()` and is walked as `for (auto const next: route)` in `ZoneList::connect()`.

The record for the commit that introduced the selector rejected this shape, and that rejection was wrong. Its argument was that a range reads well for a traversal and "this is not one — the loop stops at the first zone that yields a frame". A range-for with `break` is exactly how C++ spells an early stop, and it is what every short-circuiting loop in this tree already does; `Router`'s own walk is driven the same way. Stopping early is a property of the loop body, not of the sequence being walked, so it was never an argument about the iterator at all.

**Change**
The cursor leaves the selector and becomes the iterator's only mutable state:

```cpp
class ZoneSelector::Iterator {
  public:
    auto operator*() const -> PmZone & {
        return *m_selector->m_zones[m_cursor];
    }
    auto operator++() -> Iterator & {
        m_cursor += 1;
        settle();
        return *this;
    }
    auto operator==(Iterator const &other) const -> bool {
        return m_cursor == other.m_cursor;
    }
  private:
    friend class ZoneSelector;
    Iterator(Self const &selector, usize const cursor)
        : m_selector(&selector), m_cursor(cursor) {
        settle();
    }
    auto settle() -> void {
        while (m_cursor < m_selector->m_zones.size() &&
               !m_selector->admits(m_selector->m_zones[m_cursor])) {
            m_cursor += 1;
        }
    }
```

`settle()` runs in the constructor as well as in `operator++`, which is what makes dereferencing safe without a check: an iterator is never parked on a rejected candidate, so `begin()` either points at an admitted zone or already equals `end()`. The three filters move out of the loop into one `admits()` predicate, which is also the question the admission gate will extend.

The walk is now a range-for, and the early stop is a `break` in all but name — `return`:

```cpp
auto PmNode::get_frame_from_zone_list(AllocRequest const &request) -> PmFrame * {
    ZoneSelector const selector{m_zone_list.fallback(), request};
    for (auto &zone: selector) {
        if (auto *const frame = zone.alloc_frames(request)) {
            return frame;
        }
    }
    return nullptr;
}
```

The selector is `const` at every call site now, which is the visible payoff: it holds nothing that a walk changes.

`Iterator` keeps a `ZoneSelector const *`, not a reference, and that is deliberate even though the next commit in this series replaces the selector's own borrowed pointer with a reference. An iterator has to be assignable — `it = other` is part of what the name promises and what `std::ranges` requires — and a reference member deletes the assignment operator. A pointer inside an iterator is the idiom rather than an exception to it: `std::ranges::filter_view::iterator` holds `filter_view *parent_` for the same reason.

Rejected: a sentinel type for `end()` (`struct Sentinel {}` with a heterogeneous `operator==`), which C++20 ranges allow and which would save storing a cursor in the end iterator. It buys nothing here — the candidate list's size is known, so the end position is a number like any other — and it costs the plain `Iterator == Iterator` comparison the test's counting loop uses.

Rejected: keeping `next()` beside the range for the one caller that wants a pointer. Two ways to walk one thing is what this series exists to remove.

**Reference**
None, this project's own design. Linux's equivalent walk is driven by a macro over a raw cursor (`for_next_zone_zonelist_nodemask()` in `include/linux/mmzone.h`, advancing a `struct zoneref *z`), because C has no iterator vocabulary to use; the shape being replaced here was that macro's shape written in C++ by hand. Zircon has no zones and so no candidate walk. The local precedent that decided the shape is `PmNode::Router`, whose `begin()`/`end()` are walked as `for (auto const next: route)` at `kernel/mm/pm_node.cpp:36`.

**Verification**
`format_cpp.py --check` clean on all three files. `clang-tidy -p out/bringup.x86` on `kernel/mm/pm_node.cpp` and `kernel/commands/pm_node_test.cpp`: no diagnostic in either file or in `zone_selector.hpp` (two `modernize-use-nodiscard` findings on the new `begin()`/`end()` were fixed by marking them `[[nodiscard]]`, not suppressed).

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed
```

and the kshell tests that `boot_check.py` does not run:

```text
python3 scripts/kshell_run.py --out out/bringup.x86 pmnode.test pmzone.test

pmnode.test: ok -- 21 checks
pmzone.test: ok -- 3 checks
```

Identical to the commit before, as every commit in this series must be.

**Deliberately not done**
`PmNode::Router::clear()` has no caller and `Router` has no `for_each_node()` user outside the test; neither is this change's business. `ZoneList::for_each_zone()` and `for_each_zone_if()` keep their callback shape — they are a traversal of every zone with no criterion, which is a different job from selection, and the selector does not replace them.

The iterator is not declared to model `std::input_iterator`: that needs `difference_type`, `value_type` and a default constructor, none of which anything here asks for, and a concept nothing checks is a claim nobody verifies.

**How it was found**
None. It was raised directly as a review of the vocabulary at each seam of the layering refactor.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a `next()`-returning-null walk not distinguish "this candidate failed a filter" from "there are no more candidates", and why does a range not have to?
- Why does `settle()` have to run in the iterator's constructor and not only in `operator++`?
- Why does `Iterator` hold a `ZoneSelector const *` rather than a `ZoneSelector const &`, when the selector itself is about to switch from a pointer to a reference?

## `AllocRequest`'s immutability moves from a comment into the type (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` — the struct becomes a class: private state, accessors, a private constructor with `resolve_request()` as its only friend, and `with_nodes()`/`with_zone_range()` for a derived request
- `kernel/mm/include/ours/mm/priv/zone_selector.hpp` — reads the request through accessors
- `kernel/mm/pm_zone.cpp` — `request.gaf()`, `request.order()`
- `kernel/mm/tests/alloc_request_smoke.cpp` — the five cases read accessors
- `kernel/commands/pm_node_test.cpp` — three field assignments become derivations

**Symptom**
None observable. No value changes; what changes is which code is able to change one.

**Cause**
`AllocRequest` was a `struct` with seven public mutable fields and this comment above them:

```cpp
/// Immutable on purpose, and that is not a style preference. A value one
/// thread owns for the length of one allocation needs no synchronisation at
/// all, which is what lets the zone walk read it outside every lock.
struct AllocRequest {
    Gaf gaf;
    usize order;
    NodeMask allowed_nodes;
    ...
};
```

Nothing enforced a word of it, and the counter-example was already in the tree. `kernel/commands/pm_node_test.cpp` built a request and then edited it:

```cpp
auto others = all;
others.allowed_nodes.set(node.nid(), false);
...
auto low_only = all;
low_only.highest_zone = ZoneType::Dma;
```

Two things are wrong, and the second is the one that matters.

The small one: a comment that claims an invariant nobody checks is worse than no comment, because a later reader trusts it. The next person to need "the same request but over fewer nodes" writes the same three lines, and the one after that writes them inside a loop, and by then the request a layer below reads is not the one `resolve_request()` produced.

The real one is what "resolved" is supposed to mean. The whole reason this value exists is that the policy derived from a caller's `Gaf` was being re-derived at each layer; `resolve_request()` collapses that into one place, so every layer below can read the answer instead of recomputing it. That guarantee is only as good as the claim that *every* `AllocRequest` came out of `resolve_request()`. Public mutable fields plus a public implicit default constructor mean any caller can make one from nothing — zero-initialised, with an empty `allowed_nodes` that answers false for every node and a `lowest_zone`/`highest_zone` pair of `Dma`/`Dma` — and hand it to the walk, which would then silently find no candidate. The `Gaf::ThisNode` narrowing in particular is a rule with an easy-to-invert reading (it is a restriction, not a preference), and a hand-built request skips it entirely.

The repo's own rule says this directly: state is private and the interface is verbs; a struct whose fields are poked by whoever holds it has moved its invariant into every call site, where nobody is responsible for it.

**Change**
The fields become private with accessors, and the constructor becomes private with exactly one friend:

```cpp
class AllocRequest final {
  public:
    [[nodiscard]] constexpr auto gaf() const -> Gaf { return m_gaf; }
    ...
  private:
    friend auto resolve_request(Gaf gaf, usize order, NodeMask const &fallback, NnId home,
                                CpuNum initiator) -> AllocRequest;

    constexpr AllocRequest(Gaf const gaf, usize const order, NodeMask const &allowed_nodes,
                           NnId const home_node, CpuNum const initiator,
                           ZoneType const lowest_zone, ZoneType const highest_zone);

    Gaf m_gaf;
    ...
};
```

`resolve_request()` stays a free function rather than becoming a static factory, because the role table in `.agents/tasks/pmm-layering/DESIGN.md` gives it its own row as a pure function that owns nothing: it is the resolver, and the request is what it returns. It needs a forward declaration above the class so that the friend declaration names the namespace-scope function rather than inventing one findable only by argument-dependent lookup — `AllocRequest` appears only in the return type, so ADL would never find it.

A request that differs from another is now derived, not edited:

```cpp
    [[nodiscard]]
    constexpr auto with_nodes(NodeMask const &nodes) const -> Self {
        auto derived = *this;
        derived.m_allowed_nodes = nodes;
        return derived;
    }
```

so the test's three mutations read:

```cpp
auto without_local = all.allowed_nodes();
without_local.set(node.nid(), false);
auto const others = all.with_nodes(without_local);
...
auto const low_only = all.with_zone_range(all.lowest_zone(), ZoneType::Dma);
auto const high_only = all.with_zone_range(ZoneType::Dma32, all.highest_zone());
```

Every one of those is `const` now, which is the point: a derived request is a second value, and the request the production path reads is still the resolved one.

`with_zone_range()` takes both bounds at once rather than offering a `with_highest_zone()` and a `with_lowest_zone()`. A range with one end moved is still one fact, and two setters would let a caller build a range whose floor is above its ceiling in two steps where one call can be read in one glance.

Rejected: a `friend class PmNodeTest` or a test-only `#ifdef` to let the test keep writing fields. The test was the honest reporter here — it needed a varied request, and the type had no way to express one. Giving production code the `with_*` methods answers that need for whoever has it next; a test-only escape hatch answers it for the test and leaves the next caller to invent the mutation again.

Rejected: making `resolve_request()` a static member (`AllocRequest::resolve(...)`) and dropping the friend. It removes the forward declaration and reads fine, but it makes the resolver part of the value's interface, and the design deliberately keeps "what should this allocation be" separate from "what this allocation is" — the resolver reads the thread's policy and the machine's node state, neither of which an `AllocRequest` should know how to reach.

**Reference**
None, this project's own design, following the repo's object-orientation rule (`.agents/skills/language/modern-cpp-development`, "State is private; the interface is verbs").

Both references keep the equivalent as a plain struct, so neither is the model here. Linux's `struct alloc_context` in `mm/internal.h` has public fields written by `prepare_alloc_pages()` and then adjusted in place by `__alloc_pages_slowpath()` — `ac->nodemask` and `ac->preferred_zoneref` are both rewritten mid-allocation — which is exactly the mutable-request shape this project decided against in `DESIGN.md` ("the data fuses, the cursor splits"). Zircon has no request object at all: `PmmNode::AllocPages()` takes `(count, alloc_flags, list)` and derives nothing.

**Verification**
`format_cpp.py --check` clean on all five files. `clang-tidy -p out/bringup.x86` on `kernel/mm/pm_zone.cpp`, `kernel/mm/pm_node.cpp`, `kernel/commands/pm_node_test.cpp` and `kernel/mm/tests/alloc_request_smoke.cpp`: no diagnostic in any of them or in the two changed headers.

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed

python3 scripts/kshell_run.py --out out/bringup.x86 pmnode.test pmzone.test
pmnode.test: ok -- 21 checks
pmzone.test: ok -- 3 checks
```

**Deliberately not done**
No `with_gaf()`, `with_order()`, `with_home()` or `with_initiator()`. Those four are the caller's ask and the allocation's identity, not constraints to narrow; a path that wants a different order wants a different allocation and should resolve a new request. The two that exist are the two that name a *subset* of what the original request permitted, which is the only derivation that keeps "resolved" true of the result.

The class is not given an `operator==` or a formatter. Nothing compares or prints a request yet.

**How it was found**
`git grep` for writes to the struct's fields, prompted by the mismatch between its own comment and the absence of anything enforcing it: `kernel/commands/pm_node_test.cpp` turned up three assignments.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does "resolved" guarantee about an `AllocRequest`, and which two language-level features of the old `struct` broke that guarantee?
- Why does the friend declaration need a forward declaration of `resolve_request()` above the class, when a friend declaration normally introduces the function itself?
- Why is there a `with_nodes()` and a `with_zone_range()` but no `with_order()`?

## The zone selector's request becomes a reference, so the lifetime is in the type (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/priv/zone_selector.hpp` — `AllocRequest const *m_request` becomes `AllocRequest const &m_request`

**Symptom**
None observable. The same bytes are read through the same address; what changes is what the type permits.

**Cause**
The selector took a request by reference and immediately stored a pointer to it, under a comment telling the caller what to be careful about:

```cpp
    /// |zones| is the candidate order ... |request| must outlive the
    /// selector.
    ZoneSelector(ZoneList::Zones const zones, AllocRequest const &request)
        : m_zones(zones), m_request(&request), m_cursor(0) {}
```

That comment is the defect. A borrowed pointer is the right member when the borrow can be absent, rebound, or outlive what it points at — and none of those is true here. A selector cannot exist without a request (the constructor takes one by reference, so there is no null to store), nothing ever points it at a second request, and both live on one stack frame for the length of one allocation, the request having been resolved first. So the pointer adds three states the design does not have, and then a comment asks the reader to keep out of them.

A reference member says all of it in the declaration: not null, not rebindable, borrowed. The compiler enforces what the comment asked for, and the comment can go.

This is the same class of defect as the previous two commits in this series, one level down. There the claim "immutable" was in a comment with nothing behind it; here the claim "must outlive" was. In both cases the honest shape was already available in the language.

**Change**

```cpp
    ZoneSelector(ZoneList::Zones const zones, AllocRequest const &request)
        : m_zones(zones), m_request(request) {}
...
    /// The criterion, borrowed. A reference and not a pointer: the request is
    /// resolved before a selector can exist and outlives it on the same stack
    /// frame, so there is no selector without one and no moment at which the
    /// borrow could be rebound.
    AllocRequest const &m_request;
```

and the three reads lose an arrow: `m_request->lowest_zone()` becomes `m_request.lowest_zone()`.

What it costs is the implicitly declared copy-assignment operator, which a class with a reference member does not get. Nothing assigns a selector — `get_frame_from_zone_list()` and the five cases in `pmnode.test` each construct one and let it die — and a selector that could be reassigned to a different request mid-walk would be a bug waiting for a caller, so losing the operator is part of the point rather than a price paid for style.

`ZoneSelector::Iterator` keeps its `ZoneSelector const *m_selector` and that is not an inconsistency. An iterator is expected to be assignable (`it = other` within one walk), and a reference member would delete exactly the operation the walk's users have a right to; the previous commit's record says the same about the same member. The rule being followed is not "references beat pointers" — it is that the member should admit exactly the states its object has, and the two objects here have different ones.

**Reference**
None, this project's own design. Neither reference has a selector object to compare against: Linux keeps the cursor in the caller's loop over `ac->zonelist` with `struct alloc_context *ac` passed as a pointer because C has no other way to pass by reference, and Zircon has no zones.

**Verification**
`format_cpp.py --check` clean. `clang-tidy -p out/bringup.x86` on `kernel/mm/pm_node.cpp` and `kernel/commands/pm_node_test.cpp`: no diagnostic in either or in `zone_selector.hpp`.

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed

python3 scripts/kshell_run.py --out out/bringup.x86 pmnode.test pmzone.test
pmnode.test: ok -- 21 checks
pmzone.test: ok -- 3 checks
```

**Deliberately not done**
`ZoneList::Zones m_zones` stays a `ustl::views::Span` by value. A span is already a borrow with its own lifetime story, and it is the span's two words that are copied, not the zones.

`USTL_NO_MOVEABLE_AND_COPYABLE` was not added. The reference member already removes assignment, and copy *construction* of a selector is harmless — it yields a second range over the same candidates and the same request, with its own iterators — so there is nothing to forbid.

**How it was found**
Reading the vocabulary at each seam of the layering refactor: the comment "|request| must outlive the selector" is the kind of sentence that marks a type doing less than it could.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which three states does a pointer member admit that this design does not have, and which of them did the comment try to rule out?
- Why is a reference the right member for `ZoneSelector::m_request` but the wrong one for `ZoneSelector::Iterator::m_selector`?
- What operation does the class lose by holding a reference, and why is losing it an argument for the change rather than against it?

## `resolve_request()`'s node and CPU become one `Origin`, so a transposed call cannot compile (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` — new `Origin` record; `resolve_request()` takes it in place of two parameters, and `AllocRequest` stores it in place of two members
- `kernel/mm/pm_node.cpp` — the one production call site names its fields
- `kernel/commands/pm_node_test.cpp` — the test's request builder names its fields
- `kernel/mm/tests/alloc_request_smoke.cpp` — one `Origin` constant replaces the `HOME`/`CPU` pair at four call sites

**Symptom**
Latent, cannot be triggered today. The two call sites that existed both passed the arguments in the right order; what could not be ruled out was the next one.

**Cause**
The signature was

```cpp
auto resolve_request(Gaf gaf, usize order, NodeMask const &fallback, NnId home,
                     CpuNum initiator) -> AllocRequest;
```

and `NnId` and `CpuNum` are both aliases of `u32` (`system/public/ours/types.hpp`). To the compiler the last two parameters are the same type, so

```cpp
resolve_request(gaf, order, fallback, Cpu::id(), m_id)   // transposed
```

compiles without a word. An alias is a second spelling, not a second type; it documents intent to a reader and nothing at all to the type system. Two adjacent parameters of the same underlying type, with different meanings, is the shape where that gap turns into a defect, and here the two meanings are genuinely independent: on the path through `PmNode::alloc_frames(nid, ...)` the home node is the one the caller named, not the one the calling CPU is on, so a transposition is not caught by the two values happening to agree.

What a transposed call would have produced is not a crash but a wrong allocation, carried through the whole request. `home_node` is the node whose fallback order the walk starts from and the value `alloc_frames_postamble()` compares against to decide whether the frame was local, and `Gaf::ThisNode` narrows `allowed_nodes` to `NodeMask::of(home)` — so a CPU number arriving as the home node means the walk starts at whatever node has that number (or at none, with `ThisNode` narrowing the mask to a node that may not exist and the walk then rejecting every candidate), and the locality counters count against it. On a one-node machine, where every id involved is 0, the bug is invisible; it appears the first time the kernel runs on two nodes, which is the configuration nothing currently tests.

**Change**
One record, named for what the pair is:

```cpp
struct Origin {
    /// The node serving this allocation. Not necessarily the node |cpu| is
    /// on: a caller may name a node directly, and the thread's memory policy
    /// may choose another.
    NnId node;

    /// The CPU the request came from.
    CpuNum cpu;
};

[[nodiscard]]
inline auto resolve_request(Gaf gaf, usize order, NodeMask const &fallback, Origin origin)
    -> AllocRequest;
```

so the call site reads:

```cpp
auto const request =
    resolve_request(gaf, order, fallback, Origin{.node = m_id, .cpu = Cpu::id()});
```

Two things make this more than a tidy-up. The parameter list now has no two arguments of one type, so nothing at the call to `resolve_request()` can be transposed at all. And the one place where the two values still sit side by side — the brace that builds the `Origin` — is checked, because C++20 requires designated initialisers to appear in declaration order: `Origin{.cpu = ..., .node = ...}` is ill-formed, not merely odd. Both writers of an `Origin` in the tree use the designated form for exactly that reason.

`AllocRequest` stores the pair whole rather than unpacking it into the two members it replaced, so that the type which made the call unconfusable is the one a later reader of the class sees. `home_node()` and `initiator()` stay as the accessors and now read `m_origin.node` and `m_origin.cpu`; no `origin()` accessor was added, because nothing wants the pair back out and two ways to ask one question is what the previous commits in this series have been removing.

The host test gains one constant in place of the pair at four call sites:

```cpp
/// Deliberately a node id and a CPU number that differ, so that a request
/// which mixed the two would fail a check rather than pass by coincidence.
constexpr auto HERE = Origin{.node = HOME, .cpu = CPU};
```

`HOME` is 1 and `CPU` is 3 already, which is what makes `carried_through()`'s two checks able to tell a swap from a correct call.

Rejected: making `NnId` and `CpuNum` distinct types (a strong typedef over `u32`). It is the complete fix and it is the right one eventually — it would catch the transposition at every signature in the kernel, not just this one — but both aliases are used in several hundred places, arithmetic and array indexing included, so it is its own task with its own verification, not a step of a vocabulary pass over one module.

Rejected: reordering the parameters so the two are not adjacent. It makes a transposition look wrong rather than be wrong, which is the same comment-strength guarantee this series keeps replacing.

Rejected: `struct Origin { NnId node; CpuNum cpu; }` with private fields and accessors. There is no invariant over the pair — any node with any CPU is a legitimate origin — and the repo's own rule keeps public fields for a record its readers fill and read field by field with designated initialisers. A class here would add seven lines and guard nothing.

**Reference**
None, this project's own design. Linux passes the two separately and relies on review: `__alloc_pages_noprof()` takes `int preferred_nid` and reads the CPU inside `get_page_from_freelist()` via `zone_to_nid()`/`numa_node_id()`, so the pair never crosses one signature and the question does not arise. Zircon has neither a node id nor an initiator on its allocation path.

**Verification**
`format_cpp.py --check` clean on all four files. `clang-tidy -p out/bringup.x86` on `kernel/mm/pm_node.cpp`, `kernel/commands/pm_node_test.cpp` and `kernel/mm/tests/alloc_request_smoke.cpp`: no diagnostic in any of them or in `alloc_request.hpp`.

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed

python3 scripts/kshell_run.py --out out/bringup.x86 pmnode.test pmzone.test
pmnode.test: ok -- 21 checks
pmzone.test: ok -- 3 checks
```

**Deliberately not done**
No `Origin::current()` helper that reads `Topology::current_node()` and `Cpu::id()`. `resolve_request()` is pure on purpose — the host test is only possible because nothing inside it reads the machine — and a helper that reads both would put the impurity one call away from it and make the next caller reach for it by default.

`PmNode::alloc_frames()` keeps its `(Gaf, usize, NodeMask const &)` signature. That is the seam this task is forbidden to move, and the `Origin` is built just inside it.

**How it was found**
Reading the signature while checking the vocabulary at each seam of the layering refactor: two adjacent parameters whose types are the same alias of `u32`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `using NnId = u32;` give no protection against a transposed call, and what would?
- What exactly would go wrong, step by step, if a CPU number arrived as `home_node` on a two-node machine?
- Why is a designated initialiser (`Origin{.node = ..., .cpu = ...}`) a real check rather than a convention?

## `FrameBuddy::allocate()` answers with an `Option`, not a pointer that may be null (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/priv/frame_buddy.hpp` — `allocate()` returns `ustl::Option<PmFrame &>`; `has_frame()` and `get_frame()` go; `acquire_inner()` returns void
- `kernel/mm/frame_buddy.cpp` — the allocation loop drives the free list's own `front()`
- `kernel/mm/pm_zone.cpp` — `alloc_frame()` consumes the Option and keeps its own pointer return for now
- `kernel/mm/tests/frame_buddy_smoke.cpp` — seven call sites in five cases

**Symptom**
None observable. The same block is handed out, or not handed out, in every case.

**Cause**
`allocate()` returned `PmFrame *` and used null for "no block of this order or above is free":

```cpp
auto FrameBuddy::allocate(Order target_order) -> PmFrame * {
    for (auto order = target_order; order < NUM_ORDERS; ++order) {
        if (!has_frame(order)) {
            continue;
        }
        auto *frame = get_frame(order);
        ...
        return frame;
    }
    return nullptr;
}
```

Null-means-failure is cheap and it is also the one convention in this area that has already cost a real bug. Two days before this, `4929d8a0` fixed a null that had been wrapped in `ustl::ok()` and handed to the public interface: every `if (result)` downstream was true, because `ustl::Result`'s `operator bool` is `is_ok()` and not a test of what is inside, and the bulk and fragment loops in `PmNode` then dereferenced the null to reach `push_back()`. The pointer was not the whole cause of that — the wrapping was — but it is what made the wrapping possible to write and impossible to see: a `PmFrame *` that means "nothing" is indistinguishable, at the type level, from a `PmFrame *` that means "here it is", so nothing between the buddy and the frame list could have objected.

The deeper point is that "no block this large is free" is not an error and not an absent object. It is one of the two ordinary answers this function has, and it is the answer the caller must handle on *every* call, because a buddy allocator running out of a given order is routine. A type that forces the caller to unwrap before reading is what expresses that; a pointer leaves the forcing to review.

`ustl::Option<T &>` exists for precisely this and says so in its own comment: it holds a `T *` internally, so it costs the same word, and its only way to the referent is `value()`, which traps when there is nothing in it. There is no path from a none to a dereference.

**Change**

```cpp
auto FrameBuddy::allocate(Order target_order) -> ustl::Option<PmFrame &> {
    for (auto order = target_order; order < NUM_ORDERS; ++order) {
        auto const head = m_lists[order].front();
        if (!head) {
            continue;
        }

        auto &frame = head.value();
        remove_frame(&frame, order);
        acquire_inner(&frame, target_order, order);
        return ustl::some_ref(frame);
    }

    return ustl::NONE;
}
```

Two private helpers disappear into that, and they are the reason the conversion made the function shorter rather than longer. `has_frame(order)` was `!m_lists[order].empty()` and `get_frame(order)` was `&m_lists[order].front().value()` — a test followed by a read that asserts the test was done. `ustl::collections::intrusive::SList::front()` already returns an `Option<T &>`, so asking once answers both: an empty list cannot hand out a head, and there is no step between the two at which they could disagree. The pointer-returning `get_frame()` was in fact unwrapping an Option to produce the very pointer this change undoes.

`acquire_inner()` returns `void` now. It returned its own `frame` argument and no caller ever read it, which is a third thing a `PmFrame *` return was being used for: nothing.

`PmZone::alloc_frame()` is the only in-kernel caller and it keeps its own `PmFrame *` return for this commit — the zone's two entry points are converted in the next one, so that each commit has one layer's worth of call sites to re-verify:

```cpp
    ustl::Option<PmFrame &> taken{ustl::NONE};
    ...
    if (!taken) {
        ZoneGuard guard{m_lock};
        taken = m_buddy.allocate(order);
    }

    if (!taken) {
        return nullptr;
    }
    auto *const frame = &taken.value();
```

The dead per-CPU cache branch had to change with it: its lambda returned a bare `nullptr`, which an `Option<PmFrame &>` cannot be assigned from, so it now returns `ustl::Option<PmFrame &>{ustl::NONE}`. The branch is still unreachable (`MAX_PCPU_CACHE_ORDER` is 0) and still kept, per D4 of `.agents/tasks/pmm-layering/DESIGN.md`.

The host test's seven call sites change shape in the way that shows what was bought. Where it used to compare against null:

```cpp
    if (check(buddy.allocate(0) == nullptr, "no frames remain after drain")) {
```

it now asks the Option:

```cpp
    if (check(buddy.allocate(0).is_none(), "no frames remain after drain")) {
```

and where it checked identity it has to establish presence first, which it could previously skip:

```cpp
    auto const allocated = buddy.allocate(3);
    if (check(allocated.is_some() && &allocated.value() == &frames[0],
              "allocate(3) returns the base frame")) {
```

That extra clause is not noise. `allocated == &frames[0]` was false on a none *by luck*, because the comparison happened to be against a non-null address; a check written against a null-valued expectation would have passed on failure.

Rejected: `ustl::Result<PmFrame *, Status>` or `ktl::Result<PmFrame *>`. A Result carries why it failed, and here there is exactly one reason — no block that large — so the error type would be a single-valued enum, and the `ok(nullptr)` hazard comes straight back. `ustl::Result` is the right shape at the public boundary in `pmm.hpp`, where a caller distinguishes `OutOfMem` from `InvArgs` from `AccessDenied`; it is the wrong shape for a private mechanism with one failure mode.

Rejected: `ustl::Option<PmFrame *>`. Two levels of nothing, and `value()` would hand back a pointer that still has to be checked.

**Reference**
None, this project's own design. Linux returns a bare `struct page *` from `__rmqueue_smallest()` and tests it for null at each caller, which is the shape being replaced; Zircon's `PmmNode::AllocPage()` returns `zx_status_t` and writes the page through an out-parameter, which pushes the same hazard into the output argument. `ustl::Option`'s reference specialisation (`system/lib/ustl/include/ustl/option.hpp:243`) is documented for exactly this case and is what the conversion uses.

**Verification**
`format_cpp.py --check` clean on all four files. `clang-tidy -p out/bringup.x86` on `kernel/mm/frame_buddy.cpp`, `kernel/mm/pm_zone.cpp` and `kernel/mm/tests/frame_buddy_smoke.cpp`: no diagnostic on any changed line. Two `modernize-use-nodiscard` findings on `FrameBuddy::free_count()` and `empty()` are reported, but those two declarations are not in the diff — the new doc comment on `allocate()` moved them from lines 53-54 to 67-68, and that is all that changed about them.

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed

python3 scripts/kshell_run.py --out out/bringup.x86 pmnode.test pmzone.test
pmnode.test: ok -- 21 checks
pmzone.test: ok -- 3 checks
```

`frame-buddy-smoke` is one of those 149 and it is the one that exercises this function 500-odd times per run, including the two drain-to-empty cases.

**Deliberately not done**
`release()` keeps its `PmFrame *frame` parameter. It is an input the caller must have — there is no "free nothing" call — so an Option there would add a state the function cannot act on. The same goes for `frame_at()`, which computes an address rather than reporting a result.

`free_count()` and `empty()` keep their `usize`/`bool` returns; neither has a failure to report.

`PmZone::alloc_frames()`, `PmZone::alloc_frame()` and the zone walk's consumption of them are the next commit, and `pmm.hpp`'s free functions are deliberately untouched: the seam is what makes this affordable.

**How it was found**
None. It was raised directly as a review of the vocabulary at each seam of the layering refactor, with `4929d8a0` as the standing evidence of what the convention costs.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is `ustl::Result<PmFrame *, Status>` the wrong return type for `FrameBuddy::allocate()` when it is the right one for `mm::alloc_frames()`?
- How did replacing the return type make the function shorter, and what did `has_frame()`/`get_frame()` turn out to be doing?
- The host test's `allocate(3)` case had to gain an `is_some()` clause. Why was the old form of that check unsound rather than merely terser?

## `PmZone`'s allocation entry points answer with an `Option` too, and the seam stays where it is (2026-10-28)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `alloc_frames()` and `alloc_frame()` return `ustl::Option<PmFrame &>`
- `kernel/mm/pm_zone.cpp` — both bodies, and the zeroing step binds a reference instead of a pointer
- `kernel/mm/pm_node.cpp` — the one loop in `get_frame_from_zone_list()` that consumes a zone's answer
- `kernel/commands/pm_zone_test.cpp` — the max-order drain loop unwraps the Option
- `docs/repairs/261028/mm.md` — items 4 and 5, found while doing this and deliberately not done here

**Symptom**
None observable. The same frame is handed out, or not handed out, in every case.

**Cause**
The other half of the previous commit. `PmZone::alloc_frames()` and `alloc_frame()` returned `PmFrame *` with null for "this zone cannot serve it", which for a zone is not an edge case at all: it is the ordinary answer the whole fallback order exists to handle. A zone that is out of a given order is why `get_frame_from_zone_list()` walks on to the next candidate, so the failing answer is returned on a normal boot thousands of times, and every one of those is a pointer the type system would also have let the caller dereference.

That is the same hazard as in the buddy, one layer up, and it is the layer the `4929d8a0` bug actually travelled through: `get_frame_from_zone_list()` returned the zone's null unchanged, `PmNode::alloc_frames()` wrapped it in `ustl::ok()`, and the fragment and bulk loops read `*frame.unwrap()` because `if (result)` was true. Converting the zone is what removes the pointer from that path; converting only the buddy left it one call short.

Splitting the conversion across two commits was deliberate, for the reason the layering task is in steps at all: each commit has one layer's worth of call sites to re-verify, and if the standing test counts had moved it would be clear which layer moved them.

**Change**

```cpp
auto PmZone::alloc_frames(AllocRequest const &request) -> ustl::Option<PmFrame &> {
    if (is_below_high_wmark()) {
        return ustl::NONE;
    }

    return alloc_frame(request.gaf(), request.order());
}
```

and in `alloc_frame()` the frame is a reference from the moment it exists, so the two uses of it stop going through a pointer that was checked ten lines earlier:

```cpp
    if (!taken) {
        return ustl::NONE;
    }

    auto &frame = taken.value();
    if (!!(Gaf::Zero & gaf)) {
        std::fill_n(frame_to_virt<isize>(&frame), (usize{1} << order) * PAGE_SIZE / sizeof(usize),
                    0);
    }

    finish_allocation(&frame, gaf, order);
    return taken;
```

`PmNode`'s side is one loop, and no more than that on purpose — `PmNode`'s own return types are being changed concurrently elsewhere:

```cpp
    for (auto &zone: selector) {
        // The zone answers with an Option, which is unwrapped here because
        // PmNode's own return types are a pointer for now; this is where the
        // two conventions meet, and it is one line rather than a convention
        // of its own.
        if (auto const frame = zone.alloc_frames(request)) {
            return &frame.value();
        }
    }
```

`pmm.hpp`'s free functions are untouched, and that is the whole reason this was affordable: about 180 call sites hang off that seam, so holding it fixed is what let five commits of vocabulary change be verified by an unchanged boot each time. The two conventions now meet at exactly two places — this loop, and `PmNode::alloc_frames()` turning a null into `Status::OutOfMem` — instead of one convention running the length of the allocator.

Rejected: converting `PmNode::get_frame_from_zone_list()` and `PmNode::alloc_frames()` in the same commit. It is the obvious next step and it is somebody else's in-flight work; two sessions rewriting the same two functions is how a conflict becomes a merge of two half-converted paths.

Rejected: `PmZone::free_frame(PmFrame *frame, usize order)` taking an `Option`. It is an input the caller must have — there is no "free nothing" call — so an Option there would add a state the function cannot act on, which is the mirror image of the mistake being fixed.

**Reference**
None, this project's own design; `ustl::Option`'s reference specialisation (`system/lib/ustl/include/ustl/option.hpp:243`) is documented for this use. Linux returns a bare `struct page *` from `rmqueue()` and `get_page_from_freelist()` and tests for null at each step, which is the shape being replaced. Zircon returns `zx_status_t` with the page as an out-parameter from `PmmNode::AllocPage()`, which moves the same hazard into the argument list.

**Verification**
`format_cpp.py --check` clean on all four C++ files. `clang-tidy -p out/bringup.x86` on `kernel/mm/pm_zone.cpp`, `kernel/mm/pm_node.cpp` and `kernel/commands/pm_zone_test.cpp`: no diagnostic on any changed line. One pre-existing `readability-identifier-naming` finding on `FramePcpuCache::lists_` is reported; that declaration is not in the diff, the added include moved it from line 26 to 27.

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed

python3 scripts/kshell_run.py --out out/bringup.x86 pmnode.test pmzone.test
pmnode.test: ok -- 21 checks
pmzone.test: ok -- 3 checks
```

`pmzone.test` is the one that matters most here: it is the only test that drives `PmZone::alloc_frame()` directly, and it drains max-order blocks until the zone refuses — so it exercises the converted failure answer, not only the success one.

**Deliberately not done**
`PmNode::get_frame_from_zone_list()`, `PmNode::alloc_frames()` and the `pmm.hpp` free functions keep their pointer and `ktl::Result` returns. The first two are another session's; the third is the seam.

`PmZone::find_frame()` keeps its `PmFrame *` declaration, because it has no definition and no caller — converting a signature nobody implements is inventing an interface. Filed as item 4 of `docs/repairs/261028/mm.md`.

The unread parameters this conversion walked past — `finish_allocation()`'s `frame` and `gaf`, `alloc_frames_postamble()`'s both — are item 5 of the same file. The second of those is not only a signature question: reading `request.home_node()` and `request.initiator()` instead of re-reading the current node would change what the locality counters count, which needs its own commit and its own argument.

**How it was found**
None. It was raised directly as a review of the vocabulary at each seam of the layering refactor.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is "this zone cannot serve the request" a worse thing to spell as a null pointer than "this order's free list is empty", even though both are the same C++?
- After these two commits, at exactly which points do the Option convention and the pointer/Result convention still meet, and why is leaving them there not a half-finished conversion?
- Why does `free_frame()` keep a plain `PmFrame *` parameter when `alloc_frame()` no longer returns one?

## The node vocabulary stops saying NUMA (2026-10-28)

**Files**
- `system/public/ours/types.hpp` — `using NnId = u32` becomes `using NodeId = u32`
- `kernel/mm/include/ours/mm/cfg.hpp` — `NUMA_MAX_NODE` → `MAX_NODES`, `NUMA_MAX_NODES_BITS` → `MAX_NODES_BITS`, `NUMA_NO_NODE` → `INVALID_NODE_ID`
- 38 further files across `kernel/mm`, `kernel/lib`, `kernel/phys`, `kernel/arch`, `kernel/commands` and one `BUILD.gn` comment — 249 call sites in all

**Symptom**
None. A rename of four names, nothing else.

**Cause**
Step 1 of `.agents/tasks/numa-out-of-mm/PLAN.md`, and the user's instruction behind that design: a node is the abstraction mm is built on, and NUMA is one thing that can fill it. The names said the opposite — the id type was `NnId`, the ceiling `NUMA_MAX_NODE`, the sentinel `NUMA_NO_NODE` — so mm's most basic vocabulary asserted that a node *is* a NUMA domain.

Why first in the sequence rather than last. Every later step of that plan writes new code that names these things: the ordering derivation in `kernel/lib/numa`, the placement table in mm, the changed signatures of `ZoneList::connect()` and `PmNode::bootstrap()`. Renaming afterwards means writing each of them in a vocabulary that is about to change and then changing it; renaming first means writing the final name once. The cost of going first is one large mechanical diff, which is the cheapest kind to review.

**Change**
Four names, by word-boundary substitution over every tracked `.cpp`, `.hpp`, `.h`, `.S` and `BUILD.gn`:

| Before | After | Sites |
|---|---|---|
| `NnId` | `NodeId` | 162 |
| `NUMA_MAX_NODE` | `MAX_NODES` | 79 |
| `MAX_NODES_BITS`'s old spelling | `MAX_NODES_BITS` | 6 |
| `NUMA_NO_NODE` | `INVALID_NODE_ID` | 2 |

The word boundary is not decoration. `NUMA_MAX_NODES_BITS` has `NUMA_MAX_NODE` as a prefix — the two differ by `S_BITS` — so a plain textual substitution of the shorter name produces `MAX_NODESS_BITS`, and because that identifier then appears consistently in both its definition and its one static_assert, it would have compiled. `\bNUMA_MAX_NODE\b` does not match inside the longer name, which is why this was done with a regex rather than `sed` (BSD `sed` has no `\b`).

Rejected: renaming the build options too — `OURS_CONFIG_NUMA`, `OURS_CONFIG_NUMA_EMULATION`, `MM_NUMA`, the `mm_numa` GN argument. Those ask "does this build support more than one NUMA domain", which is a question about NUMA and is correctly named after it. What they should arguably configure is `MAX_NODES` directly rather than a NUMA flag that then sets it, which is the open question in `.agents/tasks/numa-out-of-mm/DESIGN.md` and not a rename.

**Reference**
A deliberate departure, and worth being plain about: **Linux names these after NUMA.** `include/linux/numa.h` has `MAX_NUMNODES` and `NUMA_NO_NODE`, and the id is `int nid` throughout. So this is not following the reference; it is the user's design decision that the node is the general concept here and NUMA one of its sources, taken on 2026-10-28, and the names follow that rather than Linux's.

Zircon has no counterpart: one `PmmNode` per machine and no node ids to name.

**Verification**
`format_cpp.py --check` clean on all 39 C++ files, `format_gn.py --check` clean on the one `BUILD.gn`. `git grep -nE '\bNnId\b|\bNUMA_MAX_NODE\b|\bNUMA_MAX_NODES_BITS\b|\bNUMA_NO_NODE\b'` over `kernel` and `system` comes back empty.

```text
build      ok (49.2s)
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed
```

plus `scripts/kshell_run.py --out out/bringup.x86 pmnode.test pmzone.test`, both `ok`. The 49-second build is the real evidence of the blast radius: `system/public/ours/types.hpp` is included by nearly everything, so this recompiled most of the tree and the scheduler, timers and object layers all built against the new name.

**Deliberately not done**
The build options, as above.

`INVALID_NODE_ID` was renamed and kept although it has **no code user**: the only two occurrences were its own definition and a comment in `object_cache.cpp` describing a bug that was fixed by removing the use. Renaming a dead constant is churn, but deleting it is a different change and the acceptance for this task — `git grep -i numa kernel/mm` empty — needs its name gone either way. Filed as a deletion candidate.

`MAX_NODES` still serves two jobs. It is the array ceiling, and it is also the "no node known yet" sentinel: `NODE_OF_CPU` is filled with it, and `nid >= MAX_NODES` is how callers say "no preference". So the layer has two sentinels, one of them (`INVALID_NODE_ID`, at `MAX_NODES + 1`) dead. Filed rather than fixed, because choosing one is a decision about the interface.

The `#ifndef OURS_CONFIG_NUMA_MAX_NODES` guard in `cfg.hpp` still wraps the definitions of both `MAX_NODES` and `MAX_NODES_BITS`, so defining that macro — which its name invites, as a way to pick a node count — leaves neither defined and breaks the build in a place that names neither. Filed.

**How it was found**
The user's instruction, 2026-10-28, as decision D3 of `.agents/tasks/numa-out-of-mm/DESIGN.md`.

One thing was checked before starting rather than discovered during: whether `NnId` reached userspace. It is declared in `system/public/`, which is the shared-types directory, so a rename there could in principle have been an ABI change. `git grep NnId` over `kernel/lib/userabi`, `kernel/lib/syscall` and `system/app` returns nothing — the type is kernel-internal and only its declaration sits in the public header — so the rename could not affect a syscall signature or a userspace header.

**The user's prediction**
None — homework is paused.

**Three questions you should be able to answer**
1. `NUMA_MAX_NODES_BITS` contains `NUMA_MAX_NODE`. Explain what a plain textual substitution would have produced and why the build would not have caught it.
2. Linux calls these `MAX_NUMNODES` and `NUMA_NO_NODE`. Why is renaming them here not a correction of Linux, and what makes it the right call for this kernel?
3. `MAX_NODES` is both a ceiling and a sentinel. Give the two call sites that read it each way, and say what goes wrong the day someone adds a node count equal to the ceiling.

## The node count moves beside the CPU count, and stops being a macro (2026-10-28)

**Files**
- `kernel/include/ours/node_mask.hpp` — was `kernel/mm/include/ours/mm/node_mask.hpp`; gains `MAX_NODES`, `MAX_NODES_BITS` and `INVALID_NODE_ID` as `inline constexpr`
- `kernel/mm/include/ours/mm/cfg.hpp` — loses those three
- `kernel/mm/include/ours/mm/pm_frame.hpp` — includes the new header; the drift assertion goes
- `kernel/mm/include/ours/mm/numa_topology.hpp` — includes it too, which is the point of the step
- `kernel/lib/heap/src/kmalloc.cpp` — `using ours::MAX_NODES;`
- `kernel/mm/tests/BUILD.gn` — all three host tests get `//kernel/include` on their include path; the NUMA one passes `MM_NUMA=1` instead of `OURS_CONFIG_NUMA=1`
- five mm headers and two tests — the include repointed

**Symptom**
None. The values are unchanged: `MAX_NODES` is still 32 or 1, and `MAX_NODES_BITS` still 6 or 1.

**Cause**
Step 2 of `.agents/tasks/numa-out-of-mm/PLAN.md` ran straight into a wall. `kernel/lib/numa` is to own the distance matrix, and `NumaTopology` needs `MAX_NODES` — which lived in `ours/mm/cfg.hpp`. A library under `kernel/lib/` cannot include a memory-management header to learn how many nodes there can be; that is the inversion the whole task exists to remove, pointing the other way.

So the count had to move first, and where it belongs was already answered by its twin. `MAX_CPU` is not in a subsystem: it is `inline constexpr` in `kernel/include/ours/cpu_mask.hpp`, next to `CpuMask`, derived from the build's `MP_MAX_CPUS`. `NodeMask` was the same kind of thing in the wrong place — `ours/mm/node_mask.hpp`, with its width coming from an mm config header — so anything wanting to *name* a node had to include part of the page allocator.

Two facts settled the details.

The comment justifying `MAX_NODES_BITS` as a hand-written macro said the header "is also read by assembly, which has no `bit_width()`". Six `.S` files do include `ours/mm/cfg.hpp` — `start.S`, `gdt.S`, `idt.S`, `bootstrap16.S` and two in `phys` — but **none of them names `MAX_NODES_BITS`, `MAX_NODES` or `MAX_PHYSADDR_BITS`**. `start.S` includes the header and uses nothing from it at all. So the constraint was real for the file and not for these constants, and the bit width can be computed.

And the host tests already depended on `kernel/include` without saying so: they are compiled with `-include ../../kernel/include/ours/prelude.hpp` while that directory is absent from their `-I`. The precedent for saying it is `//kernel/lib/arch:cpu-mask-smoke`, which tests `cpu_mask.hpp` and carries `include_dirs = [ "//kernel/include" ]` and `defines = [ "MP_MAX_CPUS=16" ]` on the target.

**Change**
The header moves and the three macros become constants:

```cpp
#if (defined(MM_NUMA) && MM_NUMA) || (defined(MM_NUMA_EMULATION) && MM_NUMA_EMULATION)
inline constexpr auto MAX_NODES = usize{32};
#else
inline constexpr auto MAX_NODES = usize{1};
#endif

inline constexpr auto MAX_NODES_BITS = usize{ustl::bit_width(MAX_NODES)};

inline constexpr auto INVALID_NODE_ID = NodeId{MAX_NODES + 1};

using NodeMask = ustl::BitSet<MAX_NODES, NodeId>;
```

`MAX_NODES_BITS` being computed is what removes this from `pm_frame.hpp`:

```cpp
// cfg.hpp spells MAX_NODES_BITS out by hand, because assembly reads it
// too; this is what keeps it equal to the width of MAX_NODES.
static_assert(MAX_NODES_BITS == ustl::bit_width(static_cast<usize>(MAX_NODES)));
```

That assertion's only job was to catch two hand-written `#define`s drifting apart. A value derived from another cannot drift from it, so there is nothing left to assert. The values are identical either way: `bit_width(32)` is 6 and `bit_width(1)` is 1, which is what the two branches spelled out.

`NodeMask` also leaves `ours::mm` for `ours`, beside `CpuMask`. Most uses are inside `ours::mm` and resolve through the enclosing namespace; two explicit `mm::NodeMask` qualifications in the tests were updated.

Rejected: letting `kernel/lib/numa` include `ours/mm/cfg.hpp`. It is honest about the fact that `cfg.hpp` is already everyone's header — `kernel/phys`, `kernel/arch/x86/kernel`, `kernel/lib/arch` and six `.S` files include it — and it costs nothing, but it leaves a library depending on the layer it feeds.

Rejected: moving `cfg.hpp` itself out of `ours/mm/`, which is the root fix, since it is not mm-private. It would touch every includer including the assembly, and it crosses the NUMA work: `cfg.hpp` also holds `OURS_CONFIG_NUMA` and the zone switches, so moving it is a redrawing of where memory configuration lives rather than a step of this task.

Rejected: putting the count in `system/public/ours/node_mask.hpp`, next to `NodeId`. The host tests can see `system/public` without any change, which is tempting, but a kernel build switch has no business in the header directory shared with userspace, and `MAX_CPU` sets the precedent the other way.

**Reference**
None, this project's own shape — `kernel/include/ours/cpu_mask.hpp` is the thing being matched. The nearest Linux analogue is that `MAX_NUMNODES` is in `include/linux/numa.h` and `nodemask_t` in `include/linux/nodemask.h`, neither under `mm/`, which is the same separation arrived at for the same reason.

**Verification**
`format_cpp.py --check` clean on all eleven files, `format_gn.py` clean on the one `BUILD.gn`.

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  149 passed, 0 failed
```

plus `pmnode.test: ok` and `pmzone.test: ok`.

The check that actually matters here is inside the host tests, not the counts. `alloc-request-smoke` exists to pin `Gaf::ThisNode`'s narrowing, which is only observable with more than one node, and its cases name node ids 1 and 2. Those pass, which is the proof that `MM_NUMA=1` reaches the new header and yields `MAX_NODES == 32` — see below for why that was not a given.

**Deliberately not done**
`cfg.hpp` stays under `ours/mm/` although it is not mm-private, for the reason above.

`OURS_CONFIG_NUMA` stays in `cfg.hpp`, which means the build switch is now read in two headers — the drift that `cfg.hpp`'s own top comment was written about. Chasing it turned up a latent inconsistency worth more than a tidy-up: `node_mask.hpp` counts nodes when **either** `MM_NUMA` or `MM_NUMA_EMULATION` is on, while `pm_frame.hpp` gates the frame's node field on `OURS_CONFIG_NUMA`, which follows only the first. An emulation-only build therefore gets 32 legal node ids and no field to record one in. Filed as item 6 of `docs/repairs/261028/mm.md` rather than fixed here, because the fix changes what that build compiles.

**How it was found**
The step's own blocker, but two things came out of the compiler rather than out of reading, and one was a near miss.

`kernel/lib/heap/src/kmalloc.cpp` stopped compiling: it reads `MAX_NODES` at `if (nid == MAX_NODES)` from inside `namespace heap`, which worked while the name was a macro, because a macro has no namespace. One `using ours::MAX_NODES;` fixes it, and the error is the honest consequence of the conversion rather than a problem with it.

The near miss: the first version of `node_mask.hpp` derived the count from `MM_NUMA`, and `alloc-request-smoke` forces NUMA on with `defines = [ "OURS_CONFIG_NUMA=1" ]` — a name the new header does not read. That silently reverted the test to one node and would have broken the four cases fixed yesterday for exactly that reason, reported as test failures rather than as a build error. It was masked by the build failing first, for the unrelated include-path reason. The fix is for the test to pass the switch the real build passes, `MM_NUMA=1`, which is also how `cpu-mask-smoke` passes `MP_MAX_CPUS`.

**The user's prediction**
None — homework is paused.

**Three questions you should be able to answer**
1. `kernel/lib/numa` cannot include `ours/mm/cfg.hpp`. Say why, in terms of what the library is for.
2. The `static_assert` in `pm_frame.hpp` was deleted rather than kept. What was it checking, and what makes that check impossible to fail now?
3. A host test passed `-DOURS_CONFIG_NUMA=1` to force 32 nodes, and the new header reads `MM_NUMA`. What would the symptom have been, and why would the boot counts not have shown it?
