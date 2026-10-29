# mm outstanding repairs

Found while working through `.agents/tasks/pmm-layering/PLAN.md`, each one beside the step that ran into it rather than by a scan. None of them blocks that work.

Item numbers are not reused and the remaining items are not renumbered when one is fixed, so a gap in the sequence means that item has been done and the entries in `docs/changes/` that point at a number still point at the right thing. Fixed so far: item 2 (2026-10-28, `eb9b5abb`, recorded in `docs/changes/261028/mm.md` under "Choosing a zone becomes an object"). Deleting it belonged in that commit and was missed there; the subagent that found the omission left it alone rather than sweeping another session's bookkeeping into its own commits, which was right.

## 1. The zone walk drains the scarcest zone first

**Severity**
Medium. Wrong ordering today on every allocation, but no caller can be harmed by it yet, because no caller can ask for a particular zone. It becomes a real shortage the day one can.

**Symptom**
Latent, cannot be triggered today. Once a driver needs frames from `ZoneType::Dma` or `ZoneType::Dma32`, it will find them gone: ordinary kernel and user allocations will have consumed the low zones first, while `ZoneType::Normal` still has most of its memory. On a machine where the Dma zone is the usual 16 MiB, that is a few thousand frames and it goes early in boot.

**Cause**
`ZoneList::insert_local_zone()` (`kernel/mm/include/ours/mm/pm_node.hpp:59`) stores a zone at `static_cast<usize>(ztype)` and pushes it onto the fallback order in call order, and `ZoneList::connect()` appends each remote node's zones "in type order". `ZoneIterator::move_next()` (`kernel/mm/include/ours/mm/priv/zone_iterator.hpp:57`) then walks `zones[next++]` ascending. `ZoneType::Dma` is 0, so every allocation tries Dma, then Dma32, then Normal, and takes from the first that can serve it.

That is backwards. The low zones are the only ones that can satisfy an address-constrained request; the Normal zone can satisfy everything else. Spending the constrained memory on unconstrained requests is spending the only supply that has no substitute. Linux orders the zonelist from the highest zone down for exactly this reason, and keeps `lowmem_reserve[]` on top of that so a request permitted to use a low zone still cannot take the last of it.

**Change**
Walk the permitted range from the highest zone type down, so `set_zone_range(Dma, Normal)` tries Normal first and falls back toward Dma. The iterator already carries both bounds, and `lower_zone_type` is currently unread (item 2), so this is where that bound starts meaning something.

Whether a reserve on top of the ordering is also wanted is a separate question and should not be decided here: `lowmem_reserve[]` exists because ordering alone still lets a flood of unconstrained allocations reach the low zones once Normal is empty. Worth raising with the user once a zone-constrained caller exists and the trade-off can be stated against a real consumer.

**Reference**
Linux `build_zonelists()` and `build_zonerefs_node()` in `mm/page_alloc.c`, which fill the zonelist from `MAX_NR_ZONES - 1` downwards, and `setup_per_zone_lowmem_reserve()` with the `lowmem_reserve[]` check inside `__zone_watermark_ok()`. Zircon has no counterpart: it has no zones.

**Acceptance**
A kshell test that allocates until `ZoneType::Normal` is empty and then asserts the Dma and Dma32 zones still hold most of their frames, read through `PmZone::count_free_frames()`. Both images still boot.

## 3. `set_zone_bounds()` reads two elements past the end of `max_zone_pfn`

**Severity**
Medium. Out-of-bounds reads on every boot, benign in effect today by an accident of the loop's own arithmetic, and a trap the moment libc++ hardening is on — the same class as item 3 of `docs/repairs/261026/mm.md`.

**Symptom**
Latent, cannot be triggered today. In a hardened build `ustl::views::Span::operator[]` traps and the kernel does not boot. In this build the two garbage values read are passed through `std::max(garbage, start_pfn)` after `start_pfn` has already reached `max_pfn`, and `clamp_pfn_to_zone()` then collapses the resulting span to empty, so no bogus zone is created. That is luck, not design: a garbage value is only ever used as a lower bound equal to `max_pfn`, which no node range can exceed.

**Cause**
The loop is bounded by the number of zone *types*, and the span is sized by the number of *enabled* ones.

`kernel/mm/pm_node_init.cpp:51`:

```cpp
auto set_zone_bounds(ustl::views::Span<Pfn> const max_zone_pfn) -> void {
    auto start_pfn = phys_to_pfn(EarlyMem::min_address());
    for (usize i = 0; i < std::size(ZONE_LOWER_PFN); i++) {
        auto const end_pfn = std::max(max_zone_pfn[i], start_pfn);
```

`ZONE_LOWER_PFN` is `Pfn[NR_ZONES_PER_NODE]`, and `NR_ZONES_PER_NODE` is `ZoneType::Count` = 5: Dma, Dma32, Normal, Movable, Dynamic. The span comes from `platform_init_pmm_with_config()` in `kernel/platform/pc/memory.cpp:29`, which sizes it by the template parameter pack:

```cpp
std::array<mm::Pfn, sizeof...(LinearZoneMaxPfns)> linear_zone_pfns;
```

`ArchPmmZoneConfig` lists three entries — Dma, Dma32 and Normal — so the array has three. Indices 3 and 4 are read from whatever follows it on that function's stack frame.

The `static_assert(sizeof...(LinearZoneMaxPfns) <= mm::NR_ZONES_PER_NODE)` in the same function checks the bound in the direction that cannot overflow and not the one that does. The comment above it says the design "ensures that changes to the number of zone types do not affect existing implementations", which is the intent; what it does not ensure is that the consumer stops at the number of entries it was given.

**Change**
Bound the loop by the span: `for (usize i = 0; i < max_zone_pfn.size(); i++)`. The zone types past the end then keep their zero-initialised `INIT_DATA` bounds, which `clamp_pfn_to_zone()` already reads as an empty span — the same outcome the garbage reaches today, arrived at on purpose.

Worth deciding at the same time, and for the user rather than for whoever takes this: whether a zone type with no `max_zone_pfn` entry should be an empty zone silently, or an assertion. Silence is what happens now; an assertion would have caught this. The two enabled-ness mechanisms — `ZoneType::Count` and `DisabledZoneFilter` over `ArchPmmZoneConfig` — can disagree, and nothing says which is the authority.

**Reference**
Linux's `free_area_init()` in `mm/mm_init.c` takes `max_zone_pfn[MAX_NR_ZONES]`, an array sized by the zone-type count itself, so the two cannot disagree: the arch fills every entry, including zeros for the zones it has no memory for. That is the shape this is one step away from.

**Acceptance**
`for (usize i = 0; i < max_zone_pfn.size(); i++)`, and both images boot. A hardened build would be the real check, which is also what item 3 of `docs/repairs/261026/mm.md` needs, so the two are worth taking together.

## 4. `PmZone::find_frame()` is declared and never defined

**Severity**
Low. Dead declaration, no wrong answer: nothing calls it, so nothing fails to link.

**Symptom**
Latent, cannot be triggered today. The first caller to be written gets an undefined-symbol error at link time rather than a compile error at the call, which is the least informative place to find out.

**Cause**
`kernel/mm/include/ours/mm/pm_zone.hpp` declares `auto find_frame(usize order) -> PmFrame *;` in the public interface. `git grep find_frame` finds that line and nothing else in the tree — no definition in `kernel/mm/pm_zone.cpp`, no call anywhere.

Its name says it is a lookup rather than an allocation ("find", not "alloc"), which no other zone method is, so what it was meant to do cannot be recovered from the signature: it could have been the non-removing peek that `contains()` ended up being, or an earlier draft of `alloc_frame()`. Either way it is a promise in the public interface that the class does not keep, and the question it raises — is this scaffolding for planned work, or a leftover? — is the one the repo's own rule says to settle rather than guess at.

Found while converting the zone's allocation returns to `ustl::Option<PmFrame &>` (`docs/changes/261028/mm.md`): this is the one `PmFrame *` return in `PmZone` that the conversion could not touch, because converting a signature nobody implements would be inventing an interface rather than changing one.

**Change**
Decide which it is, with the user, and then either delete the declaration or implement it. Deleting it is the recommendation: `contains()` already answers "could this zone serve an order" without removing anything, and `alloc_frame()` answers "give me one", so there is no question left for a third method to answer. If it is scaffolding, the thing to do is say in a comment what it is scaffolding for, since an empty declaration says nothing.

Not folded into the Option conversion on purpose: that change was behaviour-neutral by construction, and deleting part of a public interface is a decision about what the class is for.

**Reference**
None, this project's own design. Neither Linux nor Zircon has a non-removing per-order lookup on a zone: Linux's `__rmqueue_smallest()` and Zircon's `PmmNode::AllocPage()` both remove what they find, and the "is there one" question is answered in Linux by `zone_watermark_ok()` against counters rather than by walking the lists.

**Acceptance**
`git grep find_frame` shows either a definition with a caller, or nothing. Both images still boot with `dbg 44 passed, 4 skipped` and `rel 41 passed, 8 skipped, 6/6 required`.

## 5. Three functions on the allocation path take parameters they never read

**Severity**
Low. No wrong answer; each ignored parameter is a claim about what the function does that the body does not honour.

**Symptom**
Latent, cannot be triggered today. A reader of either signature concludes the function's result depends on arguments it does not look at, and a later change that *should* have read one has nothing to flag it.

**Cause**
Three signatures, all on the per-allocation path:

- `PmZone::finish_allocation(PmFrame *frame, Gaf gaf, usize order)` (`kernel/mm/pm_zone.cpp:115`) reads only `order`, to subtract `1 << order` from `m_free_frames`. `frame` and `gaf` are unused.
- `PmNode::alloc_frames_postamble(PmFrame *frame, AllocRequest const &request)` (`kernel/mm/pm_node.cpp:149`) reads neither: it compares `Topology::current_node()` against `m_id` and bumps one of two counters. The `request` it is handed already carries both an `initiator` and a `home_node`, which is what a locality counter should be comparing — reading the current node afresh asks a question the request has already answered, and asks it later, so a thread migrated between the two gets counted against the wrong node.
- `PmZone::alloc_frame(Gaf gaf, usize order)`'s dead per-CPU cache lambda takes `PcpuCache &cache` and returns none without touching it. That one is sanctioned scaffolding (D4 of `.agents/tasks/pmm-layering/DESIGN.md` keeps the cache), so it is listed for completeness rather than as a defect.

The first two are the same mistake at two layers: a parameter list written for what the function was going to do. In `finish_allocation()`'s case the name says it finishes an allocation while the body maintains one counter, so either the name or the parameters are wrong. In `alloc_frames_postamble()`'s case the unread `request` is the more interesting half, because reading it would also fix a correctness wrinkle rather than only a signature.

**Change**
Three separate decisions, and they should be taken with the user rather than swept:

- `finish_allocation()`: either drop to `(usize order)` and rename for what it does (it maintains the free count), or give it the work its name implies — zeroing and the frame's own bookkeeping currently sit in `alloc_frame()` around it.
- `alloc_frames_postamble()`: read `request.home_node()` and `request.initiator()` instead of `Topology::current_node()` and `m_id`, which makes both parameters live and makes the locality counters agree with the request the allocation was served under. This is a behaviour change — the counters would start counting something slightly different — so it needs its own commit and its own line in the change record, not a tidy-up.
- The cache lambda: leave it, and keep D4's reason beside it.

Ruled out: `[[maybe_unused]]` on the parameters. It silences the compiler and keeps the misleading signature, which is the opposite of what is wanted here.

**Reference**
Linux `__rmqueue_smallest()` takes `(zone, order, migratetype)` and reads all three; the free-count maintenance is a separate `__mod_zone_page_state()` call rather than a function taking a frame it ignores. For the locality counters, Linux's `zone_statistics()` in `mm/page_alloc.c` takes `(preferred_zone, z, nr_account)` and compares the two zones it was *given*, never re-reading `numa_node_id()` — which is the shape `alloc_frames_postamble()` should have.

**Acceptance**
No function on the allocation path has an unread parameter except the per-CPU cache lambda, which carries a comment saying why. If `alloc_frames_postamble()` is changed, `pmnode.test` still reports `ok` and both images boot with the standing counts.
