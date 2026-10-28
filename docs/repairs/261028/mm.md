# mm outstanding repairs

Found while reading the zone walk to decide where per-zone watermarks go (`docs/changes/261028/mm.md`). Neither item blocks that work, and neither is reachable as a wrong answer today; both are things that will be wrong the moment a caller can name a zone.

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

## 2. `ZoneIterator`'s lower zone bound is written but never read

**Severity**
Low. Dead state, no wrong answer: the only caller passes `ZoneType::Dma`, which is the lowest type there is, so an unenforced lower bound excludes nothing.

**Symptom**
Latent, cannot be triggered today. A caller that asked for, say, `set_zone_range(ZoneType::Normal, ZoneType::Normal)` to keep off the low zones would silently be served from Dma.

**Cause**
`ZoneIterator::set_zone_range()` (`kernel/mm/include/ours/mm/priv/zone_iterator.hpp:35`) stores both `lower_zone_type` and `upper_zone_type`, but `move_next()` only tests `zone->zone_type() > upper_zone_type`. Nothing reads `lower_zone_type`. The one caller, `AllocationContext::build()`, passes `Dma` as the lower bound, so the omission has never shown.

**Change**
Test both bounds in `move_next()`. It is one condition; the reason it is filed rather than done is that it is not reachable on its own and belongs with item 1, which is what gives a lower bound a purpose.

**Reference**
Linux has no lower bound to forget: `for_next_zone_zonelist_nodemask()` filters on `zone_idx(zone) > highest_zoneidx` only, because a zonelist built from the top down needs no floor. If item 1 is taken, this bound may turn out to be unnecessary here for the same reason.

**Acceptance**
`git grep lower_zone_type` shows a reader in `move_next()`, or shows the field gone. Both images still boot.
