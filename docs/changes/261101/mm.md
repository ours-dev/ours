# mm changes

## Title: one reclaim walks at a time, so a second caller cannot free a frame the first has already given back (2026-11-01)

**Files**
- `kernel/mm/include/ours/mm/memory_reclaimer.hpp`, `kernel/mm/memory_reclaimer.cpp` — `m_reclaim_lock` around the walk in `reclaim()`
- `.agents/skills/ours-kernel-design/references/mm.md` — the one-walk rule next to D3

**Symptom**
Latent. It shows up when a walk is already running and another caller enters `reclaim()`: the watchdog at OutOfMemory while the reclaim thread is still serving Warning or Critical, and `pagereclaim.test` which calls `reclaim()` itself. Nothing in a boot log reports it, and no test drives the two callers onto one page.

**Cause**
`PageQueues::adopt_first_locked()` copies out a `VmPage *` and leaves the page on the queue. The page comes off the queue only later, inside `free_page()`, under the owner's mutex. Until then the only name the walk has for that page is the frame pointer.

Two walks both copy that pointer. The second usually blocks on the owner's mutex and then finds the slot empty, and returns `Gone`. The hole is the time after the pointer is copied and before the second walk takes the mutex. The first walk frees the frame. The buddy can hand the same address out again, and a fault can install it at the same offset. `reclaim_page()` accepts it: `slot.page() != page` compares the address, and the address matches, so the walk unmaps and frees a page that was just installed. The pointer is not a generation.

Warning then OutOfMemory is the ordinary escalation, not a rare interleaving. `request()` has already woken the thread, and the next `look()` calls `reclaim()` on the watchdog thread. `pagereclaim.test` is a third caller of the same function.

**Change**
`reclaim()` takes `m_reclaim_lock` for the walk and releases it on return. A second caller blocks, then walks. A pause, or a target of 0, returns before the lock, so a test that has paused the reclaimer does not wait out a walk. The pause is checked again after the lock, so a pause taken while this caller waited still gives back nothing.

```cpp
if (paused() || frames == 0) {
    return 0;
}
Guard guard{m_reclaim_lock};
if (paused()) {
    return 0;
}
```

`request()` does not take the lock. `look()` calls it while holding the watchdog's mutex, and the walk sleeps in the owner's mutex. Taking `m_reclaim_lock` there would make every look wait the walk out, which is what `request()` exists to avoid.

OutOfMemory still walks on the watchdog's thread after it has waited. The walk it waited for was asked the Warning or Critical distance, which can be short of the OutOfMemory goal. Trusting that walk and then refusing would drop pages a second walk would have given back. Skipping the walk with a try-lock when one is in progress has the same result: the refusal happens while pages can still be given back.

The lock order on the OutOfMemory path is the watchdog's mutex, then `m_reclaim_lock`, then the owner's mutex. The reclaim thread takes only the last two. `request()` takes none of them.

**Reference**
Zircon `Evictor::EvictUntilTargetsMet()` holds `eviction_lock_` for one attempt (`zircon/kernel/vm/evictor.cc`). `EvictSynchronous()` and the eviction thread both go through it, so an OutOfMemory eviction and an asynchronous one do not walk together. `EvictAsynchronous()` only merges the target and signals.

**Verification**
- clang-format clean on the two C++ files. clang-tidy `-p out/bringup.x86` on `memory_reclaimer.cpp` reported nothing in that file.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/reclaim-one`: build ok in 43.7s. dbg PASS, shell at 5.7s, 44 passed, 4 skipped, 0 failed. rel PASS, shell at 4.6s, 41 passed, 8 skipped, 0 failed, 7/7 required. host-tests PASS, 151 passed, 0 failed.
- `python3 scripts/kshell_run.py --out out/reclaim-one pagereclaim.test mempressure.test`: pagereclaim.test ok, 18 checks; mempressure.test ok, 15 checks.
- The OutOfMemory check logged `gave back 48 frames before refusing anything` on cpu1 and then `gave back 40 of 40 frames that were asked` on cpu0. The second line is the ask the thread still held. The test passed. Whether the two walks overlapped is not something the test checks. The lock makes the second wait. That leftover ask is the next change.

**Deliberately not done**
- `m_wanted` still keeps the larger ask, and the synchronous walk does not clear it. A Warning ask the thread has not taken is still served after OutOfMemory has walked. That is the next change, not this one: this lock only stops the two walks overlapping.
- `set_free_memory_signal()` failing still makes `look()` run again at once, with no bound.
- The watchdog still holds its own mutex across the walk, so `Hold` and `set_levels()` wait for it. No production caller takes that mutex.
- Taking the page off the queue before dropping the queue lock would stop a second scan from choosing it. It would not help a caller that had already copied the pointer. One walk at a time drops the pointer before the next walk starts.
- Batching, skipping a busy owner, and an age floor stay in `docs/repairs/261030/mm.md` item 3. Nothing has measured them.

**How it was found**
A read of `MemoryReclaimer` and `MemoryWatchdog` against `PageQueues::adopt_first_locked()`, `VmCowPages::reclaim_page()`, and Zircon's `eviction_lock_`. The interleaving was not executed. The record of that read is `.agents/tasks/memory-reclaimer-watchdog/REVIEW.md`.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does `slot.page() != page` accept a frame the buddy has handed out again at the same offset?
- Why does `request()` not take `m_reclaim_lock`, when `reclaim()` does?
- Why does OutOfMemory walk again after waiting for the walk that was already running?

## Title: the watchdog's hold is a Guard, and the levels and the reclaimer are members (2026-11-01)

**Files**
- `kernel/mm/include/ours/mm/memory_watchdog.hpp`, `kernel/mm/memory_watchdog.cpp` — `Hold` keeps a `Guard<sync::Mutex>`; `MemoryWatchdog` is built with a `MemoryReclaimer &`; `MemoryPressureLevels::make()` is the only way to build a nested value
- `kernel/mm/include/ours/mm/memory_reclaimer.hpp` — `Pause` takes the reclaimer it pauses
- `kernel/commands/memory_pressure_test.cpp`, `kernel/commands/page_reclaim_test.cpp` — the call sites

**Symptom**
`MemoryWatchdog::Hold` called `m_lock.acquire()` and `release()`. Lockdep never recorded that a test held the watchdog's mutex, so an order between that mutex and another lock taken inside the hold was invisible. `look()` also named `MemoryReclaimer::get()` in the middle of the function, and `MemoryPressureLevels` was an aggregate whose methods subtracted even when the levels did not nest.

**Cause**
Lockdep's record of a held lock lives in the `Guard` that took it. `Mutex::acquire()` takes the mutex and does not push that record. A later cycle that involves the watchdog mutex and a lock taken while a `Hold` is alive has no edge for lockdep to report.
`MemoryPressureLevels` left the nesting rule in `is_valid()`, which `start()` and `set_levels()` call and `delay_level()` and `bounds_of()` do not. Those two subtract unsigned quantities. An invalid value wraps.

**Change**
`Hold` is defined in the header. Its constructor builds `Guard<sync::Mutex>` on `m_lock`, which is the acquire lockdep and the thread-safety analysis both see. The destructor calls `Guard::unlock()` and then signals. `unlock()` drops the lockdep record and the mutex before the signal, so the woken thread can take the mutex. The guard's own destructor then finds the guard already unlocked.

```cpp
explicit Hold(MemoryWatchdog &watchdog) TA_ACQUIRE(watchdog.m_lock)
    : m_watchdog(watchdog), m_guard(watchdog.m_lock) {}

~Hold() TA_RELEASE() {
    m_guard.unlock();
    m_watchdog.m_signal.signal();
}
```

`MemoryWatchdog::get()` builds the one watchdog with `MemoryReclaimer::get()`. `look()` calls that member. `Pause` takes the same kind of reference, instead of calling `get()` itself.

`MemoryPressureLevels::make()` returns `InvArgs` when the levels do not nest. `from_boot_options()` can still be invalid, and `start()` refuses it and logs the numbers. `bounds_of()` and `delay_level()` assert `is_valid()` before they subtract. The fields are private.

**Reference**
`Guard::unlock()` in `system/lib/lockdep/include/lockdep/guard.hpp`, used the same way before a wake in `kernel/kernel/sync/event.cpp` (`Event::signal`).

**Verification**
- clang-format clean on the touched C++ files. clang-tidy `-p out/bringup.x86` on `memory_watchdog.cpp`, `memory_pressure_test.cpp` and `page_reclaim_test.cpp` reported nothing in those files.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/reclaim-one`: build ok in 2.0s. dbg PASS, shell at 5.4s, 44 passed, 4 skipped, 0 failed. rel PASS, shell at 4.7s, 41 passed, 8 skipped, 0 failed, 7/7 required. host-tests PASS, 151 passed, 0 failed.
- `python3 scripts/kshell_run.py --out out/reclaim-one pagereclaim.test mempressure.test`: pagereclaim.test ok, 18 checks; mempressure.test ok, 15 checks. The OutOfMemory check still logs the watchdog's 48 frames and then the thread's 40, which is the ask left in `m_wanted`.

**Deliberately not done**
- `m_wanted` still keeps the larger ask. That is the distance having two owners, recorded in `.agents/tasks/memory-reclaimer-design/REVIEW.md`.
- `get()` and the `void *` passed to `Thread::spawn` stay. The spawn interface takes a plain function. The lambda only turns the pointer back into the object.
- The watchdog still holds its mutex across the walk.

**How it was found**
The code-shape review in `.agents/tasks/memory-reclaimer-style/REVIEW.md`. `Hold` was the only `m_lock.acquire()` in `kernel/mm`.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does `Hold` call `unlock()` before `signal()`, instead of letting the guard's destructor drop the mutex?
- Why is the `Guard` a member written in the header, rather than an `acquire()` in the `.cpp`?
- Why can `from_boot_options()` be invalid when `make()` cannot?
