# memory-reclaimer-design: review of the reclaim module's interface

**Date**: 2026-11-01

## Scope and method
Design of `MemoryReclaimer` as the watchdog uses it, after `m_reclaim_lock` landed in the working tree. Files: `kernel/mm/memory_reclaimer.cpp`, `kernel/mm/include/ours/mm/memory_reclaimer.hpp`, `kernel/mm/memory_watchdog.cpp`, `.agents/skills/ours-kernel-design/references/mm.md`, `docs/changes/261101/mm.md`.
Authorities: `ours-kernel-design` (page reclaim, D3), `codebase-design` (one interface, one owner of an invariant), and Zircon `Evictor::EvictAsynchronous` / `EvictSynchronous` / `eviction_lock_` (`zircon/kernel/vm/evictor.cc`). The earlier pass on the lock itself is `.agents/tasks/memory-reclaimer-watchdog/REVIEW.md` and the local review that found no defect in the mutex.
Read-only. The boot and the two kshell tests from 2026-11-01 are the evidence that the lock runs. They do not show that one distance was owed.

## What is done well
- `memory_watchdog.cpp:137-146`. OutOfMemory does not declare, and does not refuse, until a walk aimed at that level's upper edge has returned. That waiting rule is the one worth keeping.
- `memory_reclaimer.cpp:34-44`. `request()` returns without taking `m_reclaim_lock`. A look at Warning stays a publish and a wake.
- `memory_watchdog.cpp:148-160` and `memory_watchdog.hpp:104-106`. The armed bounds follow the free memory just read. The logged pressure lags by hysteresis. Readers of `pressure()` do not take the watchdog mutex.
- `memory_reclaimer.cpp:60-67`. Zero pages, then pager pages, each node given what is left of the one target. One order, in one function.

## High severity
### H1. The distance still owed has two owners
`memory_reclaimer.cpp:39-44` and `:71-78`, `memory_watchdog.cpp:137-149`.
The fact the module exists to hold is one number: how many frames the latest look still wants back, and whether the caller must wait until a walk of that number has finished. The interface splits it. `request()` publishes a high-water mark and returns. `reclaim()` walks the number it was given and does not read or clear `m_wanted`. `m_reclaim_lock` owns only the walk.
The ordinary escalation is the worst interleaving. `request(1000)` stores 1000 and signals. The thread has not exchanged yet. `look()` then finds OutOfMemory, takes the lock, walks its own goal, and returns. The thread then exchanges 1000 and walks that. The two walks do not overlap, which is what the lock guarantees, and the second walk is a distance the watchdog no longer means. The OutOfMemory kshell check logged both: 48 frames on the watchdog's thread, then 40 of 40 on the reclaim thread.
A high-water mark is the wrong merge for this producer. There is one, the watchdog thread, and each look replaces the distance. It does not add a second caller's delta. Zircon adds `min_pages_to_free` because many callers each bring a delta (`CombineEvictionTarget`). That is a different fact, kept under one spinlock with the rest of the target.
Two shapes keep the waiting rule and give the number one owner:
- The thread is the only walker. `request()` stores the latest distance and returns. `reclaim()` stores the latest distance and waits until the thread has finished a walk that observed it. The frame pointer, the queue, and the owner's mutex stay on that thread. A bare done-event is not enough: the thread can signal it before the waiter reaches `wait()`, so the wait has to be "the finished generation is at least the one I posted."
- Both calls keep walking, and both consume one word. OutOfMemory stores its distance over `m_wanted` before it walks, so the thread cannot afterwards exchange the old one. The mutex stays. This is the smaller patch. The watchdog thread still takes the owner's mutex.
The first is the deeper module: the watchdog learns two calls, and the walk is not part of its interface. It departs from the sentence in `look()` that the walk runs on the watchdog's thread. The observable rule, do not refuse until that goal has been walked, stays. The second keeps today's threads and only repairs the word.

## Medium severity
### M1. The watchdog mutex covers the walk
`memory_watchdog.cpp:130` and `:142`.
`m_lock` guards the levels, the hysteresis, and the declaration. `look()` holds it across `reclaim()`, which now also waits out another walk and then sleeps in the owner's mutex. `Hold` and `set_levels()` block for that whole stretch. Nothing else in production takes this mutex, so it is not a cycle. It is the wrong scope: a test that holds `Hold` to stop the watchdog arming the gate also stops it across a walk the test does not care about, and a walk that blocks keeps `set_levels()` out.
Drop the mutex around the walk, then read the levels again before arming. The declaration and the arm stay under the lock. The walk does not.

### M2. A failed arm is another look, and another walk
`memory_watchdog.cpp:121-126` and `:182-185`.
`set_free_memory_signal()` returns false when free memory has already left the bounds. `run()` calls `look()` again with no pause and no count. At OutOfMemory each of those calls enters `reclaim()`. Zircon's `WaitForMemChange()` counts these failures and does not arm from inside OutOfMemory (`memory_watchdog.cc` around the failure counter). Here the allocations are refused rather than stalled, so the deadlock that comment describes is not this one. The missing bound is. A failed arm should look again without starting a walk that the previous look already ran.

## Minor and style
- `memory_reclaimer.hpp:52-66`. `Pause` is on the public interface so a test can suppress the module. That is a test reaching past the seam. It is the same shape as `MemoryWatchdog::Hold`, and the checks need it. Leave it.

## Teaching opportunities
### T1. One number, two operations
The invariant that is not held: after `look()` returns, `m_wanted` is either 0 or the distance that look just computed.
Who writes it: only the watchdog thread, through `request()`. Who clears it: only the reclaim thread, through `exchange`. Who walks without looking at it: `reclaim()`, including the watchdog at OutOfMemory and `pagereclaim.test`.
The worst interleaving is the one in H1. The lock changes who is inside the walk. It does not change what the thread exchanges when it wakes.
A generation, if the thread becomes the only walker, is a counter the producer bumps and the thread publishes after the walk that observed it. Waiting on an event alone loses the wake that happened before `wait()`.

## Suggested follow-up order
1. Decide the owner of the distance. Recommendation: the reclaim thread is the only walker, and `reclaim()` waits for the generation it posted. The smaller patch, store the latest distance over `m_wanted` before the synchronous walk, is the alternative if the walk stays on the watchdog's thread.
2. After that, stop holding the watchdog mutex across the walk (M1), then bound a failed arm so it does not walk again (M2).
