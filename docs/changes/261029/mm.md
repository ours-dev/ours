# mm changes

## Title: the PMM gets a gate for allocations that can wait, and a free-memory signal for whoever watches memory (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/gaf.hpp` — `Gaf::CanWait`
- `kernel/mm/include/ours/mm/pmm.hpp` — `try_alloc_frames()`, `wait_for_frame_allocation()`, `set_free_memory_signal()`, `refuse_delayed_allocations()`, `approx_free_frame_count()`
- `kernel/mm/pmm.cpp` — the gate, the signal, and a check of the signal after every allocation and free; the entries that answer a bare pointer or a Status assert they are not given CanWait
- `kernel/commands/memory_pressure_test.cpp`, `kernel/commands/BUILD.gn` — `mempressure.test`, new
- `docs/repairs/261029/mm.md` — new: the cost of the signal's check on every allocation and free

**Symptom**
None: a new feature, step 1 of `.agents/tasks/memory-pressure/PLAN.md`. Before it, a frame allocation that found no frame failed on the spot, whoever asked; nothing could tell a caller to wait, and nothing told anyone that memory was getting short.

**Cause**
Why each part has the shape it has, since that is the part worth learning:

- **A gate that stays shut.** A CanWait allocation that finds free memory below the delay level shuts the gate, and every later CanWait allocation answers ShouldWait, however many frames come back, until the watcher calls `set_free_memory_signal()` again. If the gate reopened by itself as soon as free memory was back above the level, the waiters would wake, take the few frames a free gave back, push free memory under the level again and shut it again: the gate would flap at the rate of frees, and the watcher, the one thing that can make memory (reclaim) or decide that none will come (D4), might never get to act. Sticky means the gate opens only after the watcher has seen the shortage.
- **The level not above the lower bound.** The watcher's event fires when free memory falls below its lower bound. With the delay level at or below that bound, free memory has crossed the bound before the gate can shut, so a shut gate means the watcher has been told. With the level above the bound the gate could shut while the watcher sleeps, and the waiters would wait for someone who is not coming. `set_free_memory_signal()` asserts it.
- **Waiters wake one at a time.** `MAY_ALLOCATE` is an auto-unsignal event: each signal wakes one waiter, which takes its frame and signals again for the next. A manual-reset event would wake every waiter on an opening, and all of them would race for the same frames, most to find the gate shut again. A waiter that finds the gate shut again does not pass the signal on: the next opening does. A refusal is passed on, so every waiter hears it.
- **CanWait is not DirectReclaim.** DirectReclaim says the caller may block inside the call; CanWait says the call returns at once with ShouldWait and the caller waits later, where it holds no lock (D2). Only `try_alloc_frames()` takes it, because only a Result can say ShouldWait: an entry answering a null pointer would have to serve the allocation past the gate or report OutOfMem, and either is wrong without the caller knowing. Those entries assert it.
- **Above PmNode.** Every allocation and free of a frame goes through `pmm.cpp`, so the gate and the signal see all of them there without touching `PmNode`, whose allocation path the unmerged `feat/pmm-cpp-vocabulary` rewrites, and the node stays without policy about who waits (D3). The price is that there is no running free count at this layer, only the zones' own counters, summed on each check: `docs/repairs/261029/mm.md` item 1.
- **Atomics on the hot side.** What every allocation and free reads (the gate's state, the level, the bounds, the event pointer) is atomics, each loaded on its own, so the allocation path takes no new lock. The bounds and the event are two loads, not one snapshot, so a CPU may compare with bounds newer than the event it loaded; that costs at most one extra signal, which only makes the watcher look again. The event that wakes waiters and the lock ordering the watcher's calls are touched only by waiters and by the watcher.
- **Signals outside the lock.** `set_free_memory_signal()` signals `MAY_ALLOCATE` after letting go of `WATCHER_LOCK`: a signal wakes a thread and takes the scheduler's lock, which should not nest inside a PMM spinlock.

**Change**
The gate's decision, in `admit_waitable()`:

```cpp
auto state = GATE.load(std::memory_order_acquire);
while (true) {
    if (state == GateState::Shut) {
        return Status::ShouldWait;
    }
    auto const level = DELAY_LEVEL.load(std::memory_order_relaxed);
    if (level == 0 || approx_free_frame_count() >= level) {
        return Status::Ok;
    }
    if (state == GateState::Refusing) {
        return Status::OutOfMem;
    }
    if (GATE.compare_exchange_weak(state, GateState::Shut, std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
        return Status::ShouldWait;
    }
}
```

The signal, after every allocation and free, one load when nothing is armed:

```cpp
if (SIGNAL_EVENT.load(std::memory_order_acquire) == nullptr) {
    return;
}
auto const free = approx_free_frame_count();
if (free >= SIGNAL_LOWER.load(std::memory_order_relaxed) &&
    free <= SIGNAL_UPPER.load(std::memory_order_relaxed)) {
    return;
}
if (auto *const event = SIGNAL_EVENT.exchange(nullptr, std::memory_order_acq_rel)) {
    event->signal();
}
```

`approx_free_frame_count()` is public because the levels must be set from the count they are compared with: it includes the frames in the per-CPU caches, which `free_frame_count()` does not, so bounds set from the exact count can be off by as much as those caches hold.

Rejected: the PMM as a PageProvider (the user's first shape, DESIGN.md); the gate inside `PmNode` (D3 (a)); a waiting `alloc_frames()` that sleeps inside the call (Linux direct reclaim), for the reason `6794ed44` removed reclaim from the allocation path.

**Reference**
Zircon: the delayed allocations of `PmmNode` (`ShouldWaitState`, `should_wait_free_pages_level_` and `may_allocate_evt_` in `zircon/kernel/vm/include/vm/pmm_node.h:359-379`), `pmm_set_free_memory_signal()` and its rule that the delay level is not above the lower bound (`zircon/kernel/vm/include/vm/pmm.h`), `PmmNode::WaitForSinglePageAllocation()` (`zircon/kernel/vm/pmm_node.cc:339`), and the free count checked on every change in `increment_free_count_locked()` and `decrement_free_count_locked()` (`zircon/kernel/vm/pmm_node.rs`). `PMM_ALLOC_FLAG_CAN_WAIT` is `Gaf::CanWait`. Differences: Zircon keeps the gate inside its one PMM node under its lock; here it is one layer up, on atomics.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- `mempressure.test`: ok, 6 checks. `largepage.test` and `pagesource.test` still ok.
- Controls: `admit_waitable()` always answering Ok fails 3 checks (the ShouldWait, the waiter, the refusal); `set_free_memory_signal()` not signalling `MAY_ALLOCATE` fails the waiter check (it gives up after 5 seconds). Each restored, the test passes.
- clang-tidy: nothing in the files changed; clang-format clean.
- Not measured: the cost of the signal's check (repair item above).

**Deliberately not done**
- Nothing passes CanWait yet: the VMO side is step 2, the watcher step 3, and until then the gate is open with no level and no event, so every allocation behaves as before.
- The bounds are checked both ways on every call. Zircon checks the lower bound only when the count goes down and the upper only when it goes up; with the zones' counters summed either way, splitting the check saves a compare, not the sum.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why does the gate stay shut when free memory comes back above the delay level, and what would go wrong if it reopened by itself?
- Why must the delay level not be above the watcher's lower bound?
- A waiter wakes, tries, and finds the gate shut again. Why does it not signal `MAY_ALLOCATE` for the next waiter, while one that was refused does?
