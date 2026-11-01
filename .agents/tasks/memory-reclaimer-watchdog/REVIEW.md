# memory-reclaimer-watchdog: review of MemoryReclaimer and MemoryWatchdog

**Date**: 2026-11-01

## Scope and method
The two classes and the reclaim path they call: `kernel/mm/memory_reclaimer.cpp`, `kernel/mm/include/ours/mm/memory_reclaimer.hpp`, `kernel/mm/memory_watchdog.cpp`, `kernel/mm/include/ours/mm/memory_watchdog.hpp`, plus `PageQueues::adopt_first_locked()` / `reclaim()` and `VmCowPages::reclaim_page()`.
Authorities: `modern-cpp-development`, `ours-kernel-design` (`references/mm.md`, page reclaim), the accepted design `docs/design/261029/memory-pressure.md`, and the counterparts `zircon/kernel/vm/evictor.cc` (`EvictAsynchronous`, `eviction_lock_`) and `zircon/kernel/object/memory_watchdog.cc` (`WaitForMemChange`, the OOM loop in `WorkerThread`).
`python3 scripts/format_cpp.py --check` on the four files is clean. `clang-tidy -p out/bringup.x86` on the two `.cpp` files reports nothing in those files; the diagnostics it prints are in headers they include.
Every finding below is read-only judgement. Nothing was booted and no interleaving was executed.

## What is done well
- `memory_reclaimer.hpp:23-27`. Warning and Critical return at once through `request()`, and OutOfMemory calls `reclaim()` on the watchdog's own thread. That is D3 of page reclaim, and it matches the split Zircon makes between `EvictAsynchronous` and `EvictSynchronous`.
- `memory_reclaimer.cpp:34-43` and `:60-63`. The amount is published before `signal()`, and the event is `AutoUnsignal`, so a request that arrives while the thread is inside `reclaim()` stays latched for the next wait. The two fields are the right shape; a new primitive is not what is missing.
- `memory_watchdog.cpp:148-160`. The armed bounds follow the free memory just read, and a lower pressure is declared only after hysteresis. The gate opens when memory is back; the log waits.
- `memory_watchdog.hpp:104-106`. `pressure()` is an atomic written in `look()` alone, so a reader does not take `m_lock`.

## High severity
### H1. The async thread and the OutOfMemory call reclaim at the same time, and a page is named by a raw frame pointer
`memory_watchdog.cpp:137-149`, `memory_reclaimer.cpp:45-68`, `page_queues.cpp:179-191`, `vm_cow_pages.cpp:71-80`.
`request()` leaves the reclaim thread inside `PageQueues::reclaim()`. The next look, on the way down to OutOfMemory, calls `reclaim()` again on the watchdog thread. Nothing makes one wait for the other. `page_reclaim_test.cpp:614` does the same from a third caller.
`adopt_first_locked()` copies out a `VmPage *` and leaves the page on the queue. The page comes off only later, in `free_page()`, under the owner's mutex. Two callers therefore pick the same page. The second usually blocks on that mutex and then sees the slot empty (`ReclaimResult::Gone`).
The window is the gap after the pointer is copied and before `reclaim_page()` takes the mutex. The first caller frees the frame. The buddy can hand that same address back, and a fault can install it at the same offset. `reclaim_page()` then accepts it: the slot holds that address, so the check `slot.page() != page` passes, and a page that was just installed is evicted. The pointer is not a generation. This was not executed.
Zircon holds `Evictor::eviction_lock_` across one eviction attempt so a synchronous OOM eviction and the eviction thread do not run together (`evictor.h` on `eviction_lock_`, `EvictUntilTargetsMet`).
One reclaim at a time closes it. The OutOfMemory call can run the attempt itself while the thread stays out, or wait until the thread finishes the outstanding target and then look again.

## Medium severity
### M1. A later, smaller ask cannot replace an ask the thread has not taken
`memory_reclaimer.cpp:34-43` and `memory_reclaimer.hpp:41-44`.
`m_wanted` is updated only upwards. There is one producer, the watchdog thread, and each ask is the distance from the free memory just read to the upper edge of that pressure, not a delta to add to the previous ask. Two looks can happen before the thread runs: `set_free_memory_signal()` returns false, `look()` returns none, and `run()` calls `look()` again at once (`memory_watchdog.cpp:121-126`, `:182-185`).
The second distance can be smaller, because something else freed memory. The compare-and-swap keeps the first, larger one, and the thread frees that many after the pressure has already eased. The synchronous `reclaim()` does not touch `m_wanted`, so an ask posted at Warning is still there after OutOfMemory has already freed up to its own goal.
A store of the latest distance is the merge that matches one producer. The compare-and-swap is racing a second producer that does not exist. The thread's `exchange(0)` still has to win against that store, which a release store and an acquire exchange already do.

### M2. A failed arm retries look() with no bound, and each OutOfMemory retry reclaims again
`memory_watchdog.cpp:121-126` and `:182-185`.
`set_free_memory_signal()` returns false when free memory is already outside the bounds (`pmm.cpp:165-169`), which is a race with allocators. `look()` then returns none and `run()` calls it again immediately, still holding `m_lock` for the whole look. At OutOfMemory that look calls `reclaim()` again.
Zircon's `WaitForMemChange()` counts these failures and logs once the count passes 5 and is a power of two (`memory_watchdog.cc:385-415`). It also refuses to arm the signal while it is inside OutOfMemory: the comment on that function says waiting there for a memory change, before eviction, deadlocks with the stalled allocations. Here the allocations are refused rather than stalled (D4), so that deadlock is not the one to copy. The unbounded retry is.

## Minor and style
- `memory_reclaimer.cpp:46-48`. `Pause` is read at the start of `reclaim()` and `request()`. A reclaim that has already passed the check runs to its target while the pause is alive. The header says nothing is reclaimed while one is alive.
- `memory_watchdog.cpp:130` and `:137-146`. `look()` holds `m_lock` across `reclaim()`, and `reclaim()` takes the owner's mutex and can sleep in a shootdown. Production has no second taker of `m_lock`, so this is not a lock-order cycle. `Hold` and `set_levels()` block for the whole reclaim. The lock's job is the levels and the declaration; the reclaim does not read them.
- `memory_watchdog.hpp:163` and `:172`. `m_skip_hysteresis` and `m_declared_once` are two bools written under the same lock. A flags word is the convention for facts about the object's state guarded together. Nothing is wrong with the two bools.

## Teaching opportunities
### T1. Max is the wrong merge once there is only one producer
The invariant `m_wanted` is supposed to hold: the distance the latest look asked for, not yet taken by the thread.
The watchdog thread is the only caller of `request()`. The reclaim thread only exchanges the word for zero. Those two do not need a compare-and-swap that keeps the greater value. They need the store of the latest distance to happen-before the exchange that takes it, which the release store and the acquire exchange already give.
The worst interleaving of the max is: `request(1000)` stores 1000 and signals; the thread is not scheduled; the next `look()` computes 10, because free memory rose; the compare-and-swap sees 1000 and leaves it; the thread exchanges 1000 and frees 1000. A store of 10 would have left the thread with 10.
Zircon adds `min_pages_to_free` and takes the max of `free_pages_target` (`CombineEvictionTarget`) because many callers each bring their own delta. That is a different invariant, and it is why that target sits under a spinlock instead of in one atomic.

### T2. The slot check does not make a raw frame pointer a generation
`reclaim_page()` holds the owner's mutex and rejects a slot that no longer holds the pointer it was given. That catches a page that was removed and not put back. It does not catch the buddy handing the same address back into the same slot, because the address compares equal.
One reclaimer at a time never holds that pointer across someone else's `free_frames()`. Two reclaimers do. Isolating the page from the queue under the queue lock, as Zircon's `PeekIsolate` does, stops the second scan from choosing it, and still does not help a caller that already copied the pointer. The single-flight is what drops the pointer before another attempt starts.

## Suggested follow-up order
1. Make one reclaim run at a time, and decide whether OutOfMemory runs that attempt itself or waits for the thread to finish the outstanding one. This is H1.
2. Store the latest distance over `m_wanted`, and make the synchronous attempt consume or clear the outstanding ask. This is M1.
3. Bound the immediate retry when `set_free_memory_signal()` fails, and do not start another reclaim on every retry. This is M2.
4. Leave the batching, the `try_lock` of a busy owner, and the age floor alone. They are already item 3 of `docs/repairs/261030/mm.md`, and nothing has measured them. User-space pressure events and killing a process are non-goals of `docs/design/261029/memory-pressure.md`.
