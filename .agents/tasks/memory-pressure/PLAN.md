# memory-pressure: steps from a refused allocation to one that waits, watched by a watchdog thread

**Date**: 2026-10-29
**Status**: Accepted

## Request
The user, 2026-10-29: a VMO gets its pages through the page-request machinery, and something wakes reclaim workers when memory runs short; Zircon's shape (option A of the discussion), with D1 to D4 of DESIGN.md decided as recommended. Scope ends at the watchdog: eviction, compression and compaction wait for page aging. In this plan DESIGN.md means `docs/design/261029/memory-pressure.md`, where the design moved when the task finished.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Which allocations may wait | see DESIGN.md | (a) | None (homework paused) | (a), 2026-10-29 |
| D2. The flag | see DESIGN.md | (a) | None | (a), 2026-10-29 |
| D3. Where the gate and the free-memory signal live | see DESIGN.md | (b) | None | (b), 2026-10-29 |
| D4. What OutOfMemory does | see DESIGN.md | (a) | None | (a), 2026-10-29 |

## Steps
Each step is introduced to the user before it starts, and lands in its own commit.

1. **The PMM's gate.** `Gaf::CanWait`; in `pmm.cpp`, the delay level and its state, the may-allocate event, the free-memory signal, an allocation entry that answers ShouldWait (the ones answering a bare pointer refuse CanWait), `wait_for_frame_allocation()`, and the setter the watchdog will call. Why first: everything else waits on it. Verified by a kshell test that trips the gate by setting the level above free memory, and by its controls. Done: `mempressure.test`, 6 checks; both controls caught; both images boot. Also `approx_free_frame_count()`, public, since the levels must be set from the count the gate compares with (it includes the per-CPU caches); the cost of the check on every allocation and free is `docs/repairs/261029/mm.md` item 3.
2. **`AnonymousPageRequest` and the VMO.** The request, `MultiPageRequest::wait()` over both slots, `VmCowPages::alloc_pages()` taking the request's page or raising it, and the user's VMOs (`vmo_create`, a pager's VMO) created with `CanWait`. Why now: the three retry loops already wait for a page source, so a VMO page can wait once the gate exists. Verified by a kshell test: a fault, a commit and a VMO write that wait while the gate is shut and finish once it opens. Done: `mempressure.test`, 10 checks, both controls caught, both images boot. Also a preset `GAF_USER_VMO` (GAF_USER also allocates kernel objects from the heap, which cannot wait); a caller without a request is refused, not served past the gate; a 2 MiB block asks at the gate and falls back to one page.
3. **The memory watchdog.** A kernel thread, the levels as boot options, the hysteresis, the log of each change, and D4 (a) at OutOfMemory. Why last: it only sets what steps 1 and 2 read. Verified by a kshell test that moves the levels around the current free memory and checks the transitions and what a waiter gets at OutOfMemory. Done: four pressures, ImminentOutOfMemory dropped, Zircon's defaults as boot options, levels moved by `MemoryWatchdog::set_levels()` in the test (the user, 2026-10-29: 1a 2a 3a); `mempressure.test`, 15 checks, three controls caught; both images boot. Also `set_free_memory_signal()` takes Wait or Refuse, and `refuse_delayed_allocations()` is gone into it; `MemoryWatchdog::Hold` keeps the watchdog off the gate while the gate's own checks drive it.
4. Later steps are introduced one at a time.

## Files
- `kernel/mm/include/ours/mm/gaf.hpp` — `Gaf::CanWait` (step 1)
- `kernel/mm/pmm.cpp`, `kernel/mm/include/ours/mm/pmm.hpp` — the gate (step 1)
- `kernel/commands/memory_pressure_test.cpp` — new, the kshell test (steps 1 to 3)

## Test plan
- Step 1: `mempressure.test` passes: with the level above free memory a CanWait allocation answers ShouldWait and one without CanWait is served; a waiter wakes with a page when the level is reset; a free across a bound signals the watchdog's event. Controls: the gate never tripping, and the reset never waking.
- Every step: `boot_check.py`, both images.
