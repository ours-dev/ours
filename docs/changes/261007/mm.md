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
