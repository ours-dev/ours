# memory-pressure: a VMO page allocation waits through an anonymous page request, and a watchdog thread watches free memory

**Date**: 2026-10-29
**Status**: Proposed

## Problem
When the PMM has no frame, a VMO's page allocation fails on the spot: `VmCowPages::alloc_pages()` (`kernel/mm/vm_cow_pages.cpp:537`) answers OutOfMem, and the fault, commit or copy that asked fails with it. Nothing watches how much memory is free, so a user program can take the last frame, and the kernel's own next allocation (a page table, the heap) is refused too.

The user, 2026-10-29: a VMO should get its pages through the page-request machinery, with something that wakes reclaim workers when memory runs short. Their first shape, the PMM as a PageProvider that also wakes the workers, was compared with Zircon's and option A (Zircon's shape) chosen the same day.

What forced it: the user's direction, not a userspace requirement. Nothing can be reclaimed yet (no page aging: `PageQueues::start_runners()` is empty, item 9 of `docs/repairs/261026/mm.md`; no eviction, no compression), so the first steps give the waiting and the watching, and a reserve kept for the kernel; reclaim plugs in later.

## Goals and non-goals
Goals:

- `AnonymousPageRequest`, an empty placeholder in `MultiPageRequest` (`kernel/mm/include/ours/mm/page_request.hpp:240`), gets its implementation, and `MultiPageRequest::wait()` waits on whichever of its two requests was raised.
- A VMO allowed to wait (D1) answers ShouldWait instead of OutOfMem when free memory is below the level the watchdog set. The three loops that already wait for a page source, the fault (`VmAspace::soft_fault`, `kernel/mm/vm_aspace.cpp:435`), commit and the copy of a read or write (`kernel/mm/vm_object_paged.cpp:297` and `:479`), wait with no lock held and try again.
- The PMM keeps a level of free frames below which such allocations are delayed, and an event it signals when free memory crosses bounds the watchdog set.
- A memory watchdog thread turns free memory into pressure levels (Normal, Warning, Critical, ImminentOutOfMemory, OutOfMemory), logs each change, and does what D4 decides at OutOfMemory.

Non-goals:

- Eviction, compression and the thread that runs them: they need page aging first. The watchdog has the place to wake them.
- Compaction. Zircon has none; Linux's `kcompactd` exists for high-order allocations, and here only 2 MiB blocks would use it, which fall back to 4 KiB pages already.
- Waiting in the kernel's own allocations (heap, page tables, slabs): D1.
- Memory pressure events for user space (Zircon `zx_system_get_event()`), and choosing which process to kill (D4).

## Design
After Zircon: `AnonymousPageRequest` (`zircon/kernel/vm/anonymous_page_request.cc`), `PmmNode`'s delayed allocations (`zircon/kernel/vm/include/vm/pmm_node.h:359-375`, `pmm_set_free_memory_signal()` in `vm/include/vm/pmm.h`), and `MemoryWatchdog` (`zircon/kernel/object/memory_watchdog.cc`).

Objects:

- **The PMM's gate.** A level of free frames (the delay level), a state (never delay; delay once the level is tripped; delaying until reset, Zircon's `ShouldWaitState`), a may-allocate event that wakes waiters one at a time (Zircon `may_allocate_evt_`, an auto-unsignal event), and a free-memory signal: lower and upper bounds and the watchdog's event. An allocation that may wait, while the state is delaying or free frames are below the level, trips the state and answers ShouldWait. Every free checks the bounds and signals the watchdog's event when free memory leaves them. Where it lives is D3.
- **`AnonymousPageRequest`.** Active or not, and the page a wait obtained. `allocate(suspendable)` waits on the may-allocate event, with no lock held, and allocates one page for the request (Zircon `PmmNode::WaitForSinglePageAllocation()`); the retried call takes that page instead of allocating. It reports a wait every 5 seconds, and what a wait that never ends does is D4.
- **`VmCowPages::alloc_pages()`.** Takes the page the request already holds if it has one; otherwise allocates with the VMO's flags, and on ShouldWait makes the request active and answers ShouldWait. A 2 MiB block never waits: it falls back to one page, as it does today.
- **`MemoryWatchdog`.** A kernel thread. Each turn it reads free memory, works out the level, sets the PMM's free-memory signal to that level's bounds and the delay level to just below OutOfMemory, and sleeps on the signal's event; a level is left only after a hysteresis time. At OutOfMemory it does D4, then resets the delay so that waiters wake. The levels are boot options in MiB (Zircon `kernel.oom.*`); the dev image has 512 MiB.

Events, a fault when memory is short:

1. A thread faults on an anonymous page; free frames are below the delay level.
2. `VmCowPages::alloc_pages()` asks the PMM with may-wait; the PMM trips its state and answers ShouldWait; the anonymous request becomes active.
3. The fault unwinds to `VmAspace::soft_fault()`, letting go of every lock, and calls `MultiPageRequest::wait()`, which calls `AnonymousPageRequest::allocate()`.
4. The free that crossed the lower bound has already signalled the watchdog. It reaches OutOfMemory and does D4, then resets the delay; the PMM signals may-allocate.
5. The waiter wakes, allocates one page into the request, and the fault is taken again from the top, where `alloc_pages()` takes that page.

Locking: a wait holds no lock, as the page-source wait already does (the retry loops above). The gate's state is under its own spinlock, taken inside the allocator's and never around a call out of the PMM.

## Alternatives considered
- **The PMM as a PageProvider** (the user's first shape): a provider is per VMO and supplies content by offset, merging overlapping requests (`PageSource`'s outstanding set). A blank frame has no offset and no content, every anonymous VMO would need a PageSource, and only VMO allocations would ever wake reclaim. Zircon's two providers, `PagerProxy` and `PhysicalPageProvider` (`zircon/kernel/vm/include/vm/physical_page_provider.h:18-22`, the loaned frames of a contiguous VMO), both supply particular pages. Rejected by the user, 2026-10-29.
- **Reclaim inside the allocator** (Linux direct reclaim, `__alloc_pages_slowpath()`): the bottom of mm would call up into VMOs and the page queues that allocate from it. Removed for that reason in `6794ed44`.
- **Zone watermarks as the delay level**: they keep reserves per zone for callers that cannot block, which is the allocator's own concern; the delay level is a system-wide policy the watchdog sets and moves.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Which allocations may wait | (a) pages of VMOs created with a may-wait flag: the user's VMOs (Zircon `zx_vmo_create()`, `zircon/kernel/lib/syscalls/vmo.rs:54`); (b) every allocation that may block, the kernel's heap and page tables too (Linux `GFP_KERNEL`) | (a): a wait needs no lock held, and only the three retry loops let go of every lock | None (homework paused) | |
| D2. The flag | (a) a new `Gaf::CanWait`; (b) `Gaf::DirectReclaim`, which `6794ed44` left meaning "may block" for the zone gate | (a): "answer ShouldWait, the caller waits later" is not "may block here" | None | |
| D3. Where the gate and the free-memory signal live | (a) in `PmNode`'s allocation and free path, as Zircon's `PmmNode`, after merging `feat/pmm-cpp-vocabulary`, which rewrites that path (103 changed lines in `pm_node.cpp`); (b) one layer up, in `pmm.cpp`'s `alloc_frames()` and `free_frames()`, which every allocation and free goes through, comparing `free_frame_count()` with the level | (b): no merge first, and the bottom of the PMM stays without policy; the level is a heuristic, so a check and an allocation that are not one atomic step are enough | None | |
| D4. What OutOfMemory does, with nothing to reclaim | (a) fail the delayed allocations with OutOfMem and stop delaying until free memory is back above the level: the fault becomes the program's fault, a commit or copy returns OutOfMem, and the frames below the level stay the kernel's; (b) kill a process (Zircon `OomBehavior::kJobKill`, jobs with a kill-on-OOM bit; Linux's OOM killer, by footprint), which needs a policy and per-process accounting; (c) reboot or panic (Zircon `OomBehavior::kReboot`) | (a) now, (b) once user space has a policy to ask | None | |

## Testing and verification
- A kshell test that sets the levels near the current free memory, faults a VMO's pages until the gate trips, and checks that the fault waits, that the watchdog's level changes are logged, and what D4 does to the waiter; then the levels put back.
- The same for commit and for a VMO write.
- A control for each: the gate never tripping, and the watchdog never resetting it.
- `boot_check.py` for both images.
- Not verifiable yet: that waiting helps under real pressure, since nothing frees memory but a process exiting.

## Open questions
- Whether user space should get pressure events, and through what object: later, with a component that would act on them.
- What the levels' defaults should be on hardware with more than the dev image's 512 MiB.
