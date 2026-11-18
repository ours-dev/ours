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

## A synchronous walk replaces the reclaimer's pending ask through the reclaimer's own entry (2026-11-18)

**Files**
- `kernel/mm/include/ours/mm/memory_reclaimer.hpp`, `kernel/mm/memory_reclaimer.cpp` — `reclaim_superseding(frames)`; `take()` private
- `kernel/mm/memory_watchdog.cpp` — the OutOfMemory walk calls it

**Symptom**
From the review's minor list: `take()` was public only so that the watchdog could carry out the reclaimer's rule that a synchronous walk replaces the ask the thread has not taken (`auto _ = m_reclaimer.take(); m_reclaimer.reclaim(frames);`).

**Cause**
A rule about the reclaimer's own pending ask belongs in the reclaimer; a caller that has to know to call two functions in order is keeping the reclaimer's invariant for it.

**Change**

```cpp
auto MemoryReclaimer::reclaim_superseding(usize const frames) -> usize {
    auto _ = take();      // the ask from an earlier look is stale once this walk has its own count
    return reclaim(frames);
}
```

The same calls in the same order on the same thread as the watchdog made.

**Reference**
None: this kernel's own reclaimer.

**Verification**
As for the waiters' change, on the tree that holds this group.

**Deliberately not done**
Having `reclaim()` itself drop the ask (option B): tests call `reclaim()` to flush, and dropping the ask after waiting out a running walk rather than before would change the order the watchdog relies on.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 26, decided by the user on 2026-11-17 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the pending ask stale once a synchronous walk has its own count?
- Why does the drop come before the walk, not after?
- Who still calls `take()`, and why may they?

## The PMM's one call up to the reclaimer is kept, and written down (2026-11-18)

**Files**
- `.agents/tasks/pmm-redesign/DESIGN.md`, `.agents/skills/kernel-design/references/mm.md` — the reason the pipeline asks the reclaimer through its singleton while the watchdog is handed down as an event; `FrameWaiters` and `reclaim_superseding()` in the design reference

**Symptom**
From the review's minor list: `observe_pressure()` calls `MemoryReclaimer::get().request(zone)`, a call from the PMM up to a service above it, while the watchdog reaches the PMM the other way, registering an event (`set_free_memory_signal()`).

**Cause**
Not a defect to fix, the user decided (option C): `request(zone)` drops a report while a Pause lives, before it marks the node; a mark made under the PMM, as options A and B would do, would no longer be dropped but kept and taken after the Pause, which several tests and review M1's contract rely on not happening.

**Change**
Documentation only.

**Reference**
None.

**Verification**
None: documents.

**Deliberately not done**
Options A and B, for the reason above.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 29, decided by the user on 2026-11-17 (option C).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which dependency points up and which down, and why are they not the same shape?
- What would a Pause stop doing if the mark moved under the PMM?
- What would it take to make the two symmetric later?

## A request waiting for a blank frame is in one state at a time (2026-11-18)

**Files**
- `kernel/mm/include/ours/mm/page_request.hpp`, `kernel/mm/page_request.cpp` — `AnonymousPageRequest`'s `bool m_active`, `Gaf m_alloc_flags` and `PmFrame *m_frame` become one `State` (Idle, Raised, Holding), the flags and the frame sharing an anonymous union

**Symptom**
From the review's minor list: three fields could say three things at once that the request never is, raised and holding a frame, or holding one while not being asked; only assertions in `make_active()` and `allocate()` kept them apart.

**Cause**
A state machine kept as loose fields lets the invalid combinations be stored, and every reader has to know which combinations mean what. One enum names the state, and a payload that only one state has shares its storage with the other's, so a request cannot hold both.

**Change**

```cpp
enum class State : u8 { Idle, Raised, Holding };
State m_state = State::Idle;
union {
    Gaf m_alloc_flags = Gaf{};   // in Raised
    PmFrame *m_frame;            // in Holding
};
auto is_active() const -> bool { return m_state == State::Raised; }
auto has_frame() const -> bool { return m_state == State::Holding; }
```

`allocate()` copies the flags out before it leaves Raised, since the frame it gets is stored over them. The interface and every transition are as before: the caller still asks `has_frame()`, takes the frame, or raises the request on ShouldWait.

**Reference**
None: this kernel's own page requests.

**Verification**
`verify` in the `feat/pmm-redesign` worktree on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed (`admission-smoke` among them). `ktest all`: default 100 ran, 84 passed, 16 skipped; four nodes 100 ran, 96 passed, 4 skipped; failure-injection build 100 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. At 8 GiB the zones' floors still hold 14682 frames, so every watermark came out as before. clang-tidy reports nothing in `pm_zone.cpp` or `page_request.cpp`. One narrow pre-commit review covered the four changes of this group; it found the coverage list at the head of `pm_node_test.cpp` still promising the comparison item 54 removed (reworded).

**Deliberately not done**
Moving the decision "take the frame or raise the request" into the request (option B): the allocation it would make is the VMO's, with the VMO's flags and order.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 34, decided by the user on 2026-11-18 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which combinations of the three old fields were invalid, and what kept them out?
- Why must `allocate()` copy the flags before it stores the frame?
- What does a union cost here, and what would a variant have added?
