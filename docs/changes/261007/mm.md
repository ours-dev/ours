# mm changes

## Title: kernel/mm stores no fact twice: a zone asks its frame set whether it is attached, and a placement's address is optional (2026-10-07)

**Files**
- `kernel/mm/include/ours/mm/priv/frame_buddy.hpp` — `FrameBuddy::is_attached()`: whether it has a frame array
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `FrameSet::is_attached()` forwards to it; `PmZone::m_fset_attached` removed
- `kernel/mm/pm_zone.cpp` — `ensure_fset_attached()` asks `m_fset.is_attached()`
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` — `VmPlacement` holds `ustl::Option<VirtAddr>` in place of an address and `bool m_fixed`; `GapAugment::Aggregate::m_empty` says why it stays a `bool`

**Symptom**
Latent, cannot be triggered today. Three `bool` members against the rule of 2026-10-03 (modern-cpp-development): two restated a fact the object already held, and one had no comment saying why it stays.

**Cause**
The rule asks, of every `bool` member: if it restates a fact the object already holds, derive it; otherwise give it a bit of the class's Flags word; keep a lone `bool` only with a comment saying why.

- `PmZone::m_fset_attached` was set in the statement after `m_fset.attach()`, and attach() is what gives the buddy allocator its `m_frames` pointer, which a default-constructed `FrameBuddy` holds as null. So "attached" was stored twice, once as the pointer and once as the flag. Two answers to one question agree only as long as every writer remembers both; the next change that attached or detached a set another way would have had to know about the flag in the zone. The frame set now answers for itself.
- `VmPlacement::m_fixed` said whether `m_base` meant anything. That is exactly what an optional value is: an address or none. The class comment already said the shape it replaced, "a flag beside an address", was the problem; the fix had kept the shape inside the class. With `ustl::Option<VirtAddr>`, `is_fixed()` is `m_base.is_some()`, and `base()` on `anywhere()` traps through the Option in every build, where the `DEBUG_ASSERT` checked only debug builds and a release build returned 0, an address the VMM would have used.
- `GapAugment::Aggregate::m_empty` is the augmented B-tree's identity marker: a subtree with no areas, whose span fields mean nothing. It is the aggregate's only yes/no fact, so there is no word to share. A sentinel could replace it, since every non-empty aggregate has `m_min_start <= m_max_last` and an "impossible" span could stand for empty, but each reader of the aggregate would then have to know the trick. It stays, and its comment says so.

**Change**
The zone, before:

```cpp
auto PmZone::ensure_fset_attached() -> void {
    if (!m_fset_attached) {
        m_fset.attach(MemMap::get().pfn_to_frame(m_start_pfn),
                      static_cast<usize>(m_spanned_frames.load()), m_start_pfn);
        m_fset_attached = true;
    }
}
```

After:

```cpp
auto PmZone::ensure_fset_attached() -> void {
    if (!m_fset.is_attached()) {
        m_fset.attach(MemMap::get().pfn_to_frame(m_start_pfn),
                      static_cast<usize>(m_spanned_frames.load()), m_start_pfn);
    }
}
```

with `FrameBuddy::is_attached()` returning `m_frames != nullptr`.

The placement, before:

```cpp
    static constexpr auto at(VirtAddr base) -> Self {
        return Self{base, true};
    }
    constexpr auto is_fixed() const -> bool {
        return m_fixed;
    }
    constexpr auto base() const -> VirtAddr {
        DEBUG_ASSERT(m_fixed, "VmPlacement::anywhere() has no base to read");
        return m_base;
    }
    ...
    VirtAddr m_base{};
    bool m_fixed{};
```

After:

```cpp
    static constexpr auto at(VirtAddr base) -> Self {
        return Self{base};
    }
    constexpr auto is_fixed() const -> bool {
        return m_base.is_some();
    }
    constexpr auto base() const -> VirtAddr {
        return m_base.value();
    }
    ...
    ustl::Option<VirtAddr> m_base;
```

**Reference**
None, this project's own rule. Zircon's `VmAddressRegion::CreateSubVmar()` takes the address and a `VMAR_FLAG_SPECIFIC` option side by side, the shape `VmPlacement` replaced.

**Verification**
- The build of `out/bringup.x86` is clean.
- `scripts/format_cpp.py` on every changed file; `clang-tidy` on `pm_zone.cpp` and `vm_area.cpp` reports nothing on the changed lines (pre-existing `modernize-use-nodiscard` in `frame_buddy.hpp` and a `lists_` naming warning in `pm_zone.hpp`).
- Host test `frame-buddy-smoke`: OK.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required, `pc-ps2: key a` and `key A` present).
- `kshell_run.py vmaseal.test usercopy.test` on dbg: `vmaseal.test: ok -- 13 checks`, `usercopy.test: ok -- 18 checks`; both place mappings with `VmPlacement::at()`.

**Deliberately not done**
- `ensure_fset_attached()` runs on the allocation and free paths without the zone lock, so two CPUs reaching a zone's first use at once would both attach. Both write the same three values, and the first use of every zone with free frames comes during boot on one CPU, so it is latent. The derived check is not quite the same race as the flag: `attach()` writes `m_frames` first of its three fields, where the flag was written after `attach()` returned, so a second CPU could see "attached" before `m_count` and `m_base_pfn` are written. Equivalent single-threaded, which is the only way the attach is correct anyway. Making the attach happen once, at zone setup, is a change to the PMM's initialisation order, not to a member's type: `docs/repairs/261007/mm.md` item 1.
- `Aggregate` keeps its `bool`, for the reason above.

**How it was found**
`docs/repairs/261006/cpp-style.md` item 1.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What made `m_fset_attached` a second answer rather than a fact of its own, and what would have gone wrong had someone added a detach() to the buddy?
- What did a release build return from `VmPlacement::anywhere().base()` before this change, and what does it do now?
- Why does the aggregate's `m_empty` stay, although a span with start above last could stand for it?

## Title: a zone binds its frame set once, in its constructor, and the allocation and free paths stop attaching it (2026-10-07)

**Files**
- `kernel/mm/pm_zone.cpp` — the constructor builds `m_fset` bound to the zone's descriptors and asserts it has them; `ensure_fset_attached()` is gone, and with it the call at the top of `alloc_frame()`, `contains()` and `free_frame()`
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `FrameSet` loses its default constructor and `attach()`; `PmZone::m_fset` is `TA_GUARDED(m_lock)`
- `.agents/skills/ours-kernel-design/references/mm.md` — the bring-up order says where a zone's frame set is bound, and why never on first use
- `docs/repairs/261007/mm.md` — deleted: item 1 was its only item

**Symptom**
Latent, cannot be triggered today. Two CPUs reaching the first allocation from, or the first free into, a zone no CPU had attached yet would both attach it, outside the zone lock; a CPU that found the set "attached" while the other was still inside `attach()` would go on to the buddy with whatever `m_count` and `m_base_pfn` it happened to read. Every zone the handoff frees into was attached by `do_handoff()` on the boot CPU before any other CPU ran, so only a zone with every frame reserved at the handoff could get there, first used once the other CPUs are up.

**Cause**
The objects:
- `FrameBuddy`, the buddy allocator: `m_frames` (the zone's first descriptor), `m_count`, `m_base_pfn`, and one free list per order. It has no lock; `FrameSet` is a thin wrapper around it.
- `PmZone::m_lock`, the zone's spinlock, which by the comment in `alloc_frame()` covers the frame set: "the set has none of its own".
- `ensure_fset_attached()`, at the top of `alloc_frame()`, `contains()` and `free_frame()`, before any of them took `ZoneGuard`: if `m_fset.is_attached()` (`m_frames != nullptr`) was false, it called `attach()`, which writes `m_frames`, `m_count` and `m_base_pfn`, in that order in the source.

The invariant the lock protects is "the buddy's fields describe this zone". The attach wrote three of those fields with no lock held, and it was a check-then-act: the check and the act are two steps, and the act is three stores. The worst interleaving:
- CPU A finds the set unattached and enters `attach()`. It stores `m_frames`.
- CPU B finds `m_frames` set, so "attached", skips the attach, takes the zone lock and frees a frame. `release()` computes the frame's pfn as `m_base_pfn + (frame - m_frames)`: with `m_base_pfn` still 0 that is the frame's index in the zone, not its pfn, so the buddy it looks up is the wrong one; with `m_count` still 0 every lookup misses, and the block goes on the list unmerged.
- A finishes its stores. B's lock never ordered anything against them, because A held no lock.

Which of the three stores B sees is not even fixed by the source order: nothing tells the compiler another CPU reads these fields, so it may emit them in any order, and in the C++ memory model two unsynchronised accesses to one non-atomic object, one of them a write, are a data race, undefined whatever the values. Two CPUs that both attach write the same three values, which makes the duplicate attach look harmless, but the race does not depend on the values.

Why the attach was there at all: the comment on `ensure_fset_attached()` said "the memmap is populated after zones are constructed, so this runs lazily on first alloc/free". That stopped being true some time ago. `init_pmm()` now runs `build_memmap()` before `PmNode::bootstrap()`, and says why ("a PmZone binds its run of descriptors as it is constructed ... It no longer has to"), and the design reference said the reorder is what lets the lazy attach be deleted. The reason went away; the code stayed, and it kept paying for a once-only setup step with a check on every allocation, outside the lock that covers what it checks.

Binding is a fact of setup, not of use: a zone's span is fixed when it is built and its descriptors exist from `build_memmap()` on, so the binding never changes after the constructor. Moving it there removes the race rather than guarding it. `PmNode::init()` puts the zone on the node's zone list only after `new (zone) PmZone(...)` returns, so the three stores are made before any CPU, this one included, can reach the zone through anything; the zone list is built on the boot CPU before the other CPUs are started, and starting them orders everything the boot CPU wrote before.

**Change**
Before, in `pm_zone.cpp`:

```cpp
    , m_frame_cache() {}

auto PmZone::ensure_fset_attached() -> void {
    if (!m_fset.is_attached()) {
        m_fset.attach(MemMap::get().pfn_to_frame(m_start_pfn),
                      static_cast<usize>(m_spanned_frames.load()), m_start_pfn);
    }
}

auto PmZone::alloc_frame(Gaf gaf, usize order) -> PmFrame * {
    ensure_fset_attached();
    ...
auto PmZone::contains(usize order) -> bool {
    ensure_fset_attached();
    ...
auto PmZone::free_frame(PmFrame *frame, usize order) -> void {
    ensure_fset_attached();
```

After:

```cpp
    , m_frame_cache()
    , m_fset(MemMap::get().pfn_to_frame(start_pfn), end_pfn - start_pfn, start_pfn) {
    ASSERT(m_fset.is_attached(), "zone {} has no descriptor for its first pfn {:#x}", m_name,
           start_pfn);
}
```

and the three paths begin where they did after the call. In `pm_zone.hpp`, `FrameSet() = default;` and `FrameSet::attach()` are deleted, so the only way to make a frame set is bound to its frames and nothing can bind it again; and the member is annotated:

```cpp
    FrameSet m_fset TA_GUARDED(m_lock);
```

The annotation is what makes "every use of the set holds the zone lock" checked rather than promised: with `ZoneGuard` removed from `contains()` as a trial, the build stops with `reading variable 'm_fset' requires holding SpinLock 'm_lock' [-Werror,-Wthread-safety-analysis]`. The constructor's use is exempt, as clang does not analyse constructors, which is right here: the object is not shared yet.

The `ASSERT` is in every build because it runs once per zone at boot. A null descriptor for the first pfn, which the sparse memmap returns for a pfn with no section, would leave the buddy indexing off a null base, and the first free into the zone would write through it; the old code had the same hole, found later and less clearly.

The per-CPU cache step in `alloc_frame()` and `free_frame()` is untouched and still runs before the lock; nothing new runs ahead of it.

Options not taken:
- The check and the attach inside the `ZoneGuard` each path already takes. Correct, but `alloc_frame()` would have to take the lock before its per-CPU cache step, which is a stub today and is meant to run without the lock, and every allocation would still ask a question whose answer never changes.
- An atomic `m_frames` with release on the store and acquire on the check. It publishes the fields correctly, but keeps a once-only step on every allocation and adds a second protection for state the zone lock already covers; two CPUs could still both attach.
- A `DEBUG_ASSERT(m_fset.is_attached())` on the allocation and free paths. With no default constructor and no `attach()`, the set has no way to be unbound after the constructor's `ASSERT`; the check would test the type system on every call.

**Reference**
Linux sets a zone's free lists up at boot, in `free_area_init_core()` -> `init_currently_empty_zone()` -> `zone_init_free_lists()` (`mm/mm_init.c`), never on the allocation path; the one late case, memory hotplug, calls `init_currently_empty_zone()` from `move_pfn_range_to_zone()` (`mm/memory_hotplug.c`) with the hotplug lock held, not from the allocator either. Zircon sets its arenas up once, in `PmmNode::Init()` and `PmmNode::InitArena()` (`kernel/vm/pmm_node.cc`), and asserts it is early boot: `DEBUG_ASSERT(Scheduler::PeekActiveMask() == 0)` and interrupts off, with `TA_NO_THREAD_SAFETY_ANALYSIS` on both for the same reason the constructor here is exempt.

**Verification**
- The build of `out/bringup.x86` is clean, thread-safety analysis included; removing `ZoneGuard` from `contains()` as a trial makes it fail as quoted above.
- `scripts/format_cpp.py` on both files: clean. `clang-tidy` on `pm_zone.cpp`: nothing on the changed lines (the pre-existing `lists_` naming warning in `pm_zone.hpp`).
- Host tests: `frame-buddy-smoke: OK`, `protection-ranges-smoke: all tests passed`.
- `boot_check.py`: dbg PASS (35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 9 skipped, 6/6 required).
- `SMP=4 boot_check.py`: dbg PASS (35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 9 skipped, 6/6 required), CPUs 1 to 3 online in both logs. This repeats the run above rather than adding a configuration: `scripts/qemu_boot.py` already starts four CPUs when `SMP` is unset.
- `kshell_run.py pmnode.test usercopy.test vmaseal.test vmaundo.test vmawx.test vmosnapshot.test` on dbg (4 CPUs): ok -- 20, 18, 13, 20, 5 and 205 checks.
- Five boots in all, no stress loops. The vflat memmap (`mm_vflat_memmap = true`) was not built.
- Review, a NUMA build (`mm_numa = true`) under `NUMA=1`: five zones on four nodes, each bound in its constructor, eight CPUs online; dbg PASS, rel PASS.
- Review, a mutant that builds the DMA zone's set with a null base. With the `ASSERT` in place both images stop at it: `zone DMA has no descriptor for its first pfn 0x0`. With the `ASSERT` removed as well, both images pass `boot_check.py` and `pmnode.test` passes: the zone's frees still go onto its lists, they just never coalesce, and nothing at boot or in the tests looks at coalescing. So the `ASSERT` is the only check that catches an unbound zone, and it has to stay in every build.

**Deliberately not done**
- No assertion that only the boot CPU runs when a zone is built, as Zircon has. The race-freedom here does not rest on that: the zone is unreachable until its constructor returns, however many CPUs are running.
- `FrameBuddy::attach()` and its default constructor stay. Nothing in the kernel calls them now, but `FrameBuddy` is the host-testable library, and two-phase setup is a reasonable shape for it; `FrameSet`, which is what the zone holds, no longer offers it.
- Whether `FrameBuddy`'s indexing of a zone's descriptors as one array from `m_frames` holds across the sparse memmap's sections, each of which allocates its own descriptor array in `activate_section()`, was not examined. This change binds the same pointer, count and base the old code did.

**How it was found**
`docs/repairs/261007/mm.md` item 1, recorded by the change above this one, which noticed the derived "attached" check made the same race a little earlier.

**The user's prediction**
None. The user left the choice to the implementer ("你决定"), which took the item's recommended option.

**Three questions you should be able to answer**
- In the worst interleaving of the old attach, which field did the second CPU read before it was written, and what did `release()` compute from it?
- Why is binding the set in the constructor race-free without any lock or atomic, and what step in `PmNode::init()` would break that if it moved?
- Why was putting the check inside the zone lock rejected, given that it is also correct?

## Title: the buddy finds a block's partner from the descriptor it is handed, so frees coalesce in every section of a zone (2026-10-07)

**Files**
- `kernel/mm/include/ours/mm/priv/frame_buddy.hpp`, `kernel/mm/frame_buddy.cpp` — `FrameBuddy` holds the zone's pfn span instead of a base descriptor and a count; `release(frame, pfn, order)` takes the pfn and reaches the partner as `frame_at(frame, pfn, buddy_pfn)`; `attach()`, `is_attached()`, the default constructor, `contains()`, `pfn_of()`, `frame_of()` and the walk for orders above the maximum are gone; `release()` asserts the order and the alignment
- `kernel/mm/include/ours/mm/priv/sparse_memmap.hpp` — `static_assert(MAX_FRAME_ORDER + PAGE_SHIFT <= SECTION_SHIFT)`
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `FrameSet` is built from the span and `release_frame()` passes the pfn, which `PmZone::free_frame()` gets from `frame_to_pfn()`; the constructor's `ASSERT` on the first descriptor goes; `PmZone::free_blocks(order)` and `FrameSet::free_blocks(order)` count one order's free blocks
- `kernel/mm/init_pmm.cpp` — the comment on why the memmap is built before the zones no longer claims the zones bind descriptors
- `kernel/mm/object_cache.cpp`, `kernel/mm/include/ours/mm/pm_frame.hpp` — two comments that named `FrameBuddy::release_inner()`, which is now part of `release()`, name `release()`
- `kernel/commands/pm_zone_test.cpp`, `kernel/commands/BUILD.gn` — `pmzone.test`: a max-order block freed frame by frame comes back whole in the zone's first section and in a later one
- `kernel/mm/tests/frame_buddy_smoke.cpp` — the host tests pass the pfns they know; `test_two_arrays()` coalesces a block whose descriptors are a second, separately allocated array; `test_span_edges()` frees and takes every frame of a span with unaligned ends, beside descriptors just outside it that look like free partners, which only the span check keeps out; the host defines the `string_view` panic hook that a failed `ASSERT` reaches
- `.agents/skills/ours-kernel-design/references/mm.md` — the rule: never index a zone's descriptors off one base
- `docs/repairs/261007/mm.md` — deleted: item 2 was its only item

**Symptom**
Blocks freed into a zone above the memmap section that holds the zone's first pfn never merged with their partners. On the default 512 MiB image that is DMA32 from pfn 0x8000 to 0x1FEDC, three quarters of the memory: a max-order block split there by an allocation stayed split for good. Nothing failed at boot and no test looked. The new `pmzone.test`, run on the unfixed kernel, frees a max-order block one frame at a time in DMA32's first section and in its second, and reports:

```text
pmzone.test:   DMA32 zone, first section: block at pfn 0x1000 (section 0) coalesced
pmzone.test: pass  a block freed frame by frame in the zone's first section coalesces whole
pmzone.test:     order 0: 6 free block(s) before, 1030 after
pmzone.test:     order 10: 91 free block(s) before, 91 after
pmzone.test:   DMA32 zone, later section: block at pfn 0x8000 (section 1) did not coalesce
pmzone.test: FAIL  a block freed frame by frame in a later section coalesces whole
pmzone.test: FAILED -- 1 of 3 checks
```

The 1024 frames of the block at 0x8000 went onto the order-0 list as 1024 separate blocks.

**Cause**
The objects:
- A frame descriptor, `PmFrame`, 64 bytes, one per pfn. The buddy keeps free blocks on per-order lists through their first descriptor, and marks a free block's first descriptor `Role::Pmm` with its order.
- The sparse memmap, the default model. It gives each 128 MiB section (2^15 pfns) its own descriptor array, 32768 descriptors, 2 MiB, each allocated on its own by `SparseMemMap::activate_section()` from the boot allocator, which allocates top down. The dbg log's section dump on this image: section 0's array at `FFFFFF801F6C0000`, section 1's at `FFFFFF801F4C0000`, section 2's at `FFFFFF801F2C0000`, section 3's at `FFFFFF801F0C0000`. Section 1's array lies 2 MiB *below* section 0's.
- `FrameBuddy`, the buddy allocator, as it was: `m_frames`, the descriptor of the zone's first pfn; `m_count`, the zone's span; `m_base_pfn`, its first pfn. It turned a descriptor into a pfn and back by offset from `m_frames`: `pfn_of(frame)` was `m_base_pfn + (frame - m_frames)` and `frame_of(pfn)` was `&m_frames[pfn - m_base_pfn]` if the index was below `m_count`, else null.
- The DMA32 zone, `[0x1000, 0x1FEDC)`, spanning sections 0 to 3.

What `release()` did with the first frame of section 1, pfn 0x8000:
- `m_frames` = section 0's array + 0x1000 x 64 = `FFFFFF801F700000`; the frame is section 1's descriptor 0, at `FFFFFF801F4C0000`.
- `frame - m_frames` = (`...1F4C0000` - `...1F700000`) / 64 = -0x240000 / 0x40 = -0x9000.
- `pfn_of()` = 0x1000 + (usize)-0x9000 = `0xFFFFFFFFFFFF8000`, not 0x8000.
- The order-0 partner is that ^ 1, `0xFFFFFFFFFFFF8001`; `frame_of()` computes its index as `0xFFFFFFFFFFFF7001`, past `m_count`, and returns null. No partner, so the block goes on the order-0 list unmerged. The same happens at every order and for every pfn outside section 0.

`pfn_of()` treated the zone's descriptors as one array: a pfn's descriptor is `m_frames` plus its distance from the first pfn. That holds only if each section's array starts right where the previous one ends. Here they do not, and C++ says so before any layout does: subtracting two pointers into different allocations is undefined behaviour, whatever the numbers come out as. The null was luck of the layout. Had the arrays been allocated upward with a gap between them (a section-table page allocated in between, say), `frame_of()` would have returned an address inside the gap, inside `m_count`, and `release()` would have read whatever lay there as a descriptor, and unlinked it from a free list if it happened to say `Role::Pmm` and the right order. The virtually-flat memmap does lay a zone's descriptors out as one array, which is presumably the model the buddy was written against.

The fix rests on one fact: no buddy operation needs to reach further than one max-order block. A block of order `n` lies at a pfn aligned to 2^n, its partner at `pfn ^ BIT(n)`, and the two together are the aligned block of order `n + 1`; merging stops below `MAX_FRAME_ORDER`, so every pfn a free touches is inside the aligned max-order block (2^10 pfns, 4 MiB) of the frame it started from, and every split stays inside the block being split. A max-order block never crosses a section (4 MiB inside 128 MiB, both aligned), so all of its descriptors are in one section's array. So from the descriptor in hand and its pfn, the partner is `frame + (buddy_pfn - pfn)`, an offset that never leaves the array. The buddy no longer needs a base descriptor at all, only the pfn of what it is given; the zone has that from `MemMap::frame_to_pfn()`, which looks the section up through the number every descriptor stores.

**Change**
The free path, before (`frame_buddy.cpp`):

```cpp
auto FrameBuddy::release_inner(PmFrame *frame, Pfn pfn, Order order) -> void {
    while (order < MAX_ORDER) {
        auto const buddy_pfn = pfn ^ BIT(order);
        auto *buddy = frame_of(buddy_pfn);
        if (!buddy || !is_mergeable(frame, buddy, order)) {
            break;
        }
        ...
auto FrameBuddy::release(PmFrame *frame, Order order) -> void {
    auto pfn = pfn_of(frame);
    auto const end_pfn = pfn + BIT(order);

    // Orders beyond the maximum are released in max-order chunks.
    order = std::min(order, MAX_ORDER);
    while (pfn != end_pfn) {
        release_inner(frame, pfn, order);
        pfn += BIT(order);
        frame = frame_of(pfn);
    }
}
```

After:

```cpp
auto FrameBuddy::release(PmFrame *frame, Pfn pfn, Order order) -> void {
    ASSERT(order <= MAX_ORDER, "order {} is above the top order {}", order, MAX_ORDER);
    ASSERT((pfn & (BIT(order) - 1)) == 0, "pfn {:#x} is not aligned to its order {}", pfn, order);
    DEBUG_ASSERT(spans(pfn) && pfn + BIT(order) <= m_end_pfn, ...);

    while (order < MAX_ORDER) {
        auto const buddy_pfn = pfn ^ BIT(order);
        if (!spans(buddy_pfn)) {
            break;
        }
        auto *const buddy = frame_at(frame, pfn, buddy_pfn);
        if (!is_mergeable(frame, buddy, order)) {
            break;
        }
        ...
```

with

```cpp
    static auto frame_at(PmFrame *frame, Pfn pfn, Pfn other) -> PmFrame * {
        return frame + static_cast<isize>(other - pfn);
    }
```

and the zone supplying the pfn (`pm_zone.cpp`):

```cpp
        auto const pfn = frame_to_pfn(frame);
        ZoneGuard guard{m_lock};
        m_fset.release_frame(frame, pfn, order);
```

The zone builds its set from its span, `m_fset(start_pfn, end_pfn)`, where it used to pass `MemMap::get().pfn_to_frame(start_pfn)`, the count and the base. The span check `spans(buddy_pfn)` is what replaces `m_count`: a partner outside the zone's span is another zone's frame or none, and the head of a free block in the span is in the same section as the frame in hand, so its descriptor exists. `sparse_memmap.hpp` pins the fact the offset rests on:

```cpp
    static_assert(MAX_FRAME_ORDER + PAGE_SHIFT <= SECTION_SHIFT,
                  "a max-order block must not cross a memmap section");
```

It sits in the sparse model because that is the model whose per-section arrays make it necessary; the vflat model's descriptors are one virtual array and set no such limit.

What went with the base descriptor:
- The walk in `release()` for orders above `MAX_ORDER` stepped from one max-order chunk to the next with `frame_of()`, crossing sections for any order above 15. Nothing frees above `MAX_FRAME_ORDER` (`PmNode::alloc_frames()` refuses such an order and the handoff caps at it), so it became an `ASSERT` rather than a walk through the memmap.
- `contains(PmFrame const *)` compared descriptor addresses against `[m_frames, m_frames + m_count)`, the same one-array assumption; nothing called it.
- `attach()`, `is_attached()` and the default constructor. The record above this one kept `attach()` and the default constructor as a reasonable two-phase shape for the host-testable library; what they attached was the descriptor array, which no longer exists. `FrameSet::is_attached()` and the `ASSERT` in `PmZone`'s constructor that the first pfn has a descriptor went for the same reason: there is no base for a null to poison.

The two `ASSERT`s are in every build, as each is one comparison and each guards a silent corruption: an order above the top indexes past the list array, and a misaligned pfn flips its bit into the middle of another block, whose descriptor would then be unlinked as a free partner. The span check is `DEBUG_ASSERT`, as a frame reaches a zone through its own descriptor's zone type. A probe on the host (a temporary `release(&f[1], 1, 1)` in the smoke test) stops with `PANIC: Assertion failed on (pfn & (BIT(order) - 1)) == 0 with message: pfn 0x1 is not aligned to its order 1`.

Option not taken (the repair item's second option): one `FrameBuddy` per contiguous run of descriptors, several per zone. Correct, but every free would first have to find its run and every allocation walk several sets. Ruled out earlier: making the boot allocator place a zone's section arrays adjacent and in order, which depends on allocation order and is still pointer arithmetic across separate allocations.

**Reference**
Linux finds the buddy relative to the page in hand: `find_buddy_page_pfn()` (`mm/internal.h`) computes `__find_buddy_pfn(pfn, order)` and returns `page + (__buddy_pfn - pfn)`, called from `__free_one_page()` (`mm/page_alloc.c`), which is handed the pfn by its caller rather than working it out, and checks `VM_BUG_ON_PAGE(pfn & ((1 << order) - 1), page)` and `bad_range(zone, page)` first, in debug builds only. `include/linux/mmzone.h` refuses the build when `(MAX_PAGE_ORDER + PAGE_SHIFT) > SECTION_SIZE_BITS` ("Allocator MAX_PAGE_ORDER exceeds SECTION_SIZE"), which is what keeps that offset inside one section's `mem_map`. The zone's `frame_to_pfn()` is classic sparsemem's `__page_to_pfn()` (`include/asm-generic/memory_model.h`): the section number from the page, then the page's distance from that section's `mem_map`. Zircon gives each `PmmArena` one contiguous `page_array_` for exactly its range and indexes only within it, `get_page()` and `get_index()` in `kernel/vm/include/vm/pmm_arena.h`.

**Verification**
- Against the unfixed buddy (the test and `free_blocks()` built on the parent commit's `FrameBuddy`, in this worktree, before the fix was written): `pmzone.test` FAILED -- 1 of 3 checks, as quoted under Symptom.
- With the fix, `out/bringup.x86`: the build is clean; `scripts/format_cpp.py --check` is clean on every changed file; `clang-tidy` reports nothing on changed lines once `spans()` got its `[[nodiscard]]` (the pre-existing `modernize-use-nodiscard` on `free_count()`, `empty()` and `has_frame()`, and the `lists_` naming warning in `pm_zone.hpp`, remain).
- Host tests: `frame-buddy smoke: OK` (including `test_two_arrays()` and `test_span_edges()`), `protection-ranges-smoke: all tests passed`. Built against a copy of the fixed `frame_buddy.cpp` with the `spans()` check deleted, every other test still passes and `test_span_edges()` traps (SIGTRAP, the intrusive list erasing a descriptor that is on no list): it is the one test of the check that took `m_count`'s place, since no zone on these images has an end that is not max-order aligned next to a descriptor that says free.
- `boot_check.py`: dbg PASS (35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 9 skipped, 6/6 required), 4 CPUs.
- `kshell_run.py pmzone.test pmnode.test vmosnapshot.test vmaundo.test` on dbg: ok -- 3, 20, 205 and 20 checks. `pmzone.test` now reports `DMA32 zone, later section: block at pfn 0x8000 (section 1) coalesced`.
- A NUMA build (`mm_numa = true`, `NUMA=1`: four nodes, 1024 MiB, 8 CPUs): `boot_check.py` dbg PASS (35 passed, 3 skipped, 0 failed), rel PASS (33 passed, 9 skipped, 6/6 required); `pmzone.test: ok -- 9 checks`, a first-section and a later-section block in each of the four multi-section zones (two DMA32, two Normal, sections 0 to 7), and `pmnode.test: ok`.
- The vflat memmap (`mm_vflat_memmap = true`) was not built: it does not link at the parent commit either, as `VflatMemMap::pfn_to_frame()`, `frame_to_pfn()` and the rest are declared and never defined. `frame_buddy.cpp`, `pm_zone.cpp`, `init_pmm.cpp`, `early_mem.cpp`, `memmap_init.cpp` and `pm_zone_test.cpp` compile (`-fsyntax-only`) with `MM_VFLAT_MEMMAP=true`, every one of them but `frame_buddy.cpp`, which includes no memmap, then including `vflat_memmap.hpp` in place of the sparse header.
- Twelve boots in all, no stress loops: one dbg boot for `pmzone.test` on the unfixed buddy; on the fix, two `boot_check.py` runs (four boots) and two `kshell_run.py` runs on `out/bringup.x86`, the last on the final build; on the NUMA build, two `boot_check.py` runs (four boots, the second only to read the dbg verdict line again) and one `kshell_run.py`.
- The review, before and after its fixups (the two comments and `test_span_edges()`): `boot_check.py` dbg and rel PASS each time, and `kshell_run.py pmzone.test pmnode.test vmosnapshot.test vmaundo.test` ok each time; six more boots, on `out/bringup.x86` only.

**Deliberately not done**
- Performance. `PmZone::free_frame()` now calls `frame_to_pfn()` on every free: under the sparse model that is the descriptor's section number and a walk of the section table, three dependent loads with a presence check at each level, where the old code did one (wrong) pointer subtraction. It runs outside the zone lock. Not measured, and not acted on: whether it matters, and whether to cache a section's array base in the zone or prefer the vflat model, where it is a subtraction, is a question for the user.
- `pmzone.test` reads the zone's per-order counts before and after the frees, each under the zone lock but not as one snapshot, so another CPU allocating from the zone in between would move them. The shell is idle when it runs; it says so in its file comment rather than adding a zone API that reads a descriptor under the lock.
- To reach a later section the test takes max-order blocks from the zone until one lies outside the first section (at most a section's worth, 32), and gives the others back untouched. On this image the second one taken was already at 0x8000.

**How it was found**
`docs/repairs/261007/mm.md` item 2, found by the review of the change above this one: a probe asserting `FrameBuddy::contains(frame)` in `PmZone::free_frame()` stopped the rel image at the handoff on pfn 0x8000.

**The user's prediction**
None. The user chose the recommended option, finding the partner relative to the descriptor in hand, on 2026-10-07; no prediction was asked for.

**Three questions you should be able to answer**
- For the DMA32 zone on this image, what pfn did the old `pfn_of()` compute for the first frame of section 1, and why did that make every merge there miss rather than corrupt something?
- Why can the buddy find any partner it needs from the descriptor in hand, and which `static_assert` would have to fail before that stopped being true?
- What did the walk for orders above `MAX_ORDER` in the old `release()` assume, and why is an assertion the right replacement here rather than a walk through the memmap?
