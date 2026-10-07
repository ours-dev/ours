# mm outstanding repairs

## 1. A zone attaches its frame set on first use without its lock

**Severity**
Medium: a data race, but not reached today: it needs two CPUs to reach the first use of one zone at once.

**Symptom**
Latent, cannot be triggered today. `PmZone::ensure_fset_attached()` runs at the top of `alloc_frame()`, `contains()` and `free_frame()`, before `ZoneGuard` is taken. Two CPUs that both find the frame set unattached both call `FrameBuddy::attach()`; one that finds it attached while the other is still inside `attach()` goes on to the buddy with whatever `m_count` and `m_base_pfn` it happens to read.

**Cause**
The zone's lock covers the frame set ("the set has none of its own", `pm_zone.cpp`), but the attach, which writes the set, is made outside it. Today every zone that holds free frames is attached by `do_handoff()`, which frees every range bootmem still calls free into the buddy on the boot CPU before any other CPU runs, so no second CPU ever sees a zone mid-attach. A zone that has no free frame at the handoff, every frame of it reserved, is attached later by whichever CPU first allocates from it or frees into it, possibly after the other CPUs are up.

Since 2026-10-07 (`docs/changes/261007/mm.md`) "attached" is read off `FrameBuddy::m_frames`, which `attach()` writes first of its three fields, where the old `m_fset_attached` flag was written after `attach()` returned. Both are races; the derived check only moves the point at which a second CPU may see "attached" before the rest of the attach. That does not make the race worse in a way that matters, but it means the derived check is equivalent to the flag only single-threaded, which is the only way the attach is correct anyway.

**Change**
To decide with the user, since it sets where the attach happens and under which lock:

- Recommended: attach each zone's frame set once, where the zone is set up after the memmap exists, and drop `ensure_fset_attached()` from the allocation and free paths. "Attached" is a fact of setup, not of use, and the hot paths stop asking.
- Or: make the check and the attach inside the `ZoneGuard` each path already takes. Correct, and the paths that take the lock anyway pay nothing more, but `alloc_frame()` would have to take it before its per-CPU cache step, which is a stub today and is meant to run without it.

Ruled out: an atomic flag with acquire and release. It makes the publication correct but keeps a once-only setup step on every allocation, and adds a second protection for state the zone lock already covers.

**Reference**
Linux sets a zone's free lists up at boot, in `init_currently_empty_zone()` and `zone_init_free_lists()`, never on the allocation path. Zircon sets its arenas up once, in `PmmNode::Init()` and `PmmNode::InitArena()` (`kernel/vm/pmm_node.cc`), in early boot before any other CPU runs, and asserts as much (`Scheduler::PeekActiveMask() == 0`).

**Acceptance**
No path reads or writes the frame set outside the zone's lock, or the attach is made once before any second CPU runs and the paths no longer make it; `boot_check.py` passes, and `SMP=4` boots of both images pass.
