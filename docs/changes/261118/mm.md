# mm changes

## The gate's "outside the bounds" test is written once (2026-11-18)

**Files**
- `kernel/mm/pmm.cpp` — `FreeMemoryGate::outside(free, lower, upper)`, asked by `check_signal()` of the stored bounds and by `set()` of the bounds it is about to store

**Symptom**
From the review's minor list: the test that decides whether the watcher's event is signalled was written twice, once with `>=` and `<=` negated and once with `<` and `>`, so a change to one could miss the other.

**Cause**
One rule, two spellings. A static helper with the bounds as parameters serves both, since one site asks it of the stored bounds and the other of the new ones.

**Change**

```cpp
static auto outside(usize free, usize lower, usize upper) -> bool { return free < lower || free > upper; }
```

**Reference**
None: this kernel's own gate.

**Verification**
Recorded with the waiters' change, on the tree that holds both.

**Deliberately not done**
A bounds structure in the public `set_free_memory_signal()` (option B): it would break the rule that the public PMM interface takes primitive parameters (the user's, 2026-11-10).

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 35, decided by the user on 2026-11-17 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `outside()` take the bounds as parameters rather than read the stored ones?
- What would a bounds structure in the public interface have cost?
- When is the watcher's event signalled, and by whom?

## The waiters for a frame are a class of their own, which keeps the pass-on rule (2026-11-18)

**Files**
- `kernel/mm/pmm.cpp` — `FrameWaiters` owns the waiters' event, `wake_one()` and `wait_then_retry()`; `FreeMemoryGate` loses `wait_for_opening()`, `pass_on()` and its event; `set()`, `free_block()` and `adopt_frames()` wake one waiter; `wait_for_frame_allocation()` waits and retries through it

**Symptom**
From the review's minor list: the gate had three jobs, admission, the watcher's signal and the waiters' queue, and the rule that a woken waiter passes the wake on unless it was put off again lived in a free function (`wait_for_frame_allocation()`), while two more free functions and the gate signalled the event.

**Cause**
The waiters are woken by more than the gate: a zone climbing back over its Low line wakes one too (review N1). A queue several owners wake, with a rule for passing a wake on, is an object of its own; the gate keeps only what is the gate's, and wakes a waiter when it is armed again.

**Change**

```cpp
class FrameWaiters final {
  public:
    auto wake_one() -> void;
    template <typename Retry>
    auto wait_then_retry(time::Deadline const &, task::Interruptible, Retry &&retry) -> ktl::Result<PmFrame *>;
  private:
    sync::Event m_wake{sync::Event::Options::AutoUnsignal};
};

auto wait_for_frame_allocation(...) -> ktl::Result<PmFrame *> {
    return WAITERS.wait_then_retry(deadline, interruptible, [alloc_flags] { return alloc_frames(alloc_flags); });
}
```

The same event, the same wakes in the same places, and the same rule: a waiter that was served, or refused, passes the wake on; one put off again with `ShouldWait` does not.

**Reference**
Linux has no such queue: its allocations retry in `__alloc_pages_slowpath()` and wait in reclaim or `congestion_wait()`-style throttles. None, this kernel's own gate.

**Verification**
On the tree that holds the four changes of this group, in the `feat/pmm-redesign` worktree: `verify` on the default and the four-node machine, dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed. `ktest all`: default 100 ran, 84 passed, 16 skipped (a first run, while the reviewers loaded the machine, took 98 s against the usual 54 s and failed three timing tests outside mm, `Hotplug.TakesACpuOfflineAndBringsItBackTwice`, `Timer.ForwardLatenessHoldOffAndRestartCancel` and `TimerShare.NotStarvedByASlowerTimer`; run again on an idle machine, all passed); four nodes 100 ran, 96 passed, 4 skipped; failure-injection build 100 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. clang-tidy reports nothing in the changed sources. A narrow pre-commit review found two blank lines left in `memory_reclaimer.hpp` (one deleted), the design reference still naming `take()` as the watchdog's call, and the recorded reason for keeping item 29 put imprecisely (both corrected; two of its verifiers lost their connections, and those two findings were checked by hand).

**Deliberately not done**
Per-node waiter queues (option C): a departure from DESIGN.md.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 27, decided by the user on 2026-11-17 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which events wake a waiter, and why does a waiter pass the wake on?
- Why is a waiter put off again not to pass it on?
- What did the gate keep, and why is waking a waiter still something it does?
