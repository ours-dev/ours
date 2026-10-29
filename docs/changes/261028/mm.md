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
