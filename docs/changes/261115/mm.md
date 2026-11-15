# mm changes

## The frame's packed word is page::StatusWord (2026-11-15)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `page::Status` renamed `page::StatusWord`, with a comment saying what the word holds

**Symptom**
Inside `namespace ours::mm::page`, an unqualified `Status` named the frame's packed bit-field word, while every mm operation, `pin_frame()` and `unpin_frame()` among them, returns `ours::Status`. Nothing misused it yet; a function added to `page::` that returned `Status` would have returned a bit-field word.

**Cause**
The alias was named for what it describes (the frame's status) at a time when nothing in `page::` returned a result. The name hides the outer one by C++'s ordinary scope lookup: the innermost declaration wins.

**Change**
`using StatusWord = ustl::BitFields<...>`, and the member `page::StatusWord m_status`. The member keeps its name: it is the frame's status, and only the type collided.

**Reference**
None: a name of this codebase's own.

**Verification**
`verify` in the `feat/pmm-redesign` worktree on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed.

**Deliberately not done**
None.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor findings, naming.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a type alias in a nested namespace hide one with the same name in an enclosing namespace, and how would `ours::Status` still be reached?
- Why rename the type and not the member?
- What would a compiler have said if a `page::` function had declared `-> Status` and returned `Status::Ok`?

## resolve_request's node set is |nodes|, and three test locals say what they hold (2026-11-15)

**Files**
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` — `resolve_request()`'s parameter `fallback` is `nodes`, in the friend declaration and the definition
- `kernel/tests/pm_node_test.cpp` — `pinned` is `home_excluded` (a ThisNode request whose node set leaves out its home) and `this_node`, `pinned_failed` is `this_node_failed`, `invalid` is `fragment_refused`

**Symptom**
`resolve_request(..., NodeMask const &fallback, ...)` receives the caller's allowed node set, which every caller names `nodes`, while "fallback" already means the zone order `ZoneList::fallback()`. In the tests, `pinned` named a request confined by `Gaf::ThisNode`, since `pin_frame()` gave "pin" a meaning of its own, and `invalid` held a check for `Status::OutOfMem`, left from when the expected answer was `InvArgs`.

**Cause**
Names kept through changes in what they held: the mask was once the fallback order's input, the test requests were "pinned to a node" before frames could be pinned, and the fragment check changed its expected status without changing its name.

**Change**
The renames above; no behaviour changes.

**Reference**
None.

**Verification**
`verify` on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed (`alloc-request-smoke` among them); `ktest sel=PmNode.*` on four nodes 11 ran, 10 passed, 1 skipped.

**Deliberately not done**
None.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor findings, naming.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What two different things would "fallback" have meant in one call from `alloc_block()` to `resolve_request()`?
- Why is a stale name a defect here rather than a cosmetic issue?
- Which test check named `invalid` would have read as a bug report had it failed?

## The anonymous-page wait carries task::Interruptible, not a bool (2026-11-15)

**Files**
- `kernel/mm/include/ours/mm/pmm.hpp`, `kernel/mm/pmm.cpp` — `wait_for_frame_allocation()` and `FreeMemoryGate::wait_for_opening()` take `task::Interruptible`; the gate's bool-to-enum conversion goes
- `kernel/mm/include/ours/mm/page_request.hpp`, `kernel/mm/page_request.cpp` — `AnonymousPageRequest::allocate()` takes `task::Interruptible`; `MultiPageRequest::wait()` converts its `suspendable` bool once, with a comment saying the two words mean the same
- `kernel/tests/memory_pressure_test.cpp` — the three calls pass `task::Interruptible::Yes` or `No` instead of `true` or `false`

**Symptom**
One fact had two names and a type that said neither: `MultiPageRequest::wait(bool suspendable)` passed it to `AnonymousPageRequest::allocate(bool interruptible)`, then to `wait_for_frame_allocation(..., bool interruptible)`, and the gate turned the bool back into the `task::Interruptible` the event takes. A call read `wait_for_frame_allocation(flags, deadline, true)`.

**Cause**
The anonymous-page wait was added beside the page-source wait, whose provider interface (`PageProvider::wait_on_event(event, bool suspendable)`) already carried the bool, and the bool was passed along.

**Change**
The mm side of the anonymous-page wait takes the enum from `MultiPageRequest::wait()` down to the event. The page-source side keeps its `bool suspendable`: it is a provider interface with implementations outside mm, and changing it is a separate step.

**Reference**
Zircon's `PageRequest::Wait(bool suspendable)` uses the same word for the same thing; the enum is this kernel's own (`kernel/include/ours/task/types.hpp`).

**Verification**
`verify` on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed; the failure-injection build builds. `ktest all` on the default machine 90 ran, 83 passed, 7 skipped; `ktest sel=MemoryPressure.*:Pager*:PageSource*:Vmo*` 7 ran, 7 passed.

**Deliberately not done**
- `PageProvider::wait_on_event()` and `MultiPageRequest::wait()` still take `bool suspendable`.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor findings, naming.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does a bare `true` at a call site fail to tell its reader that `task::Interruptible::Yes` does?
- Why convert once at `MultiPageRequest::wait()` rather than change the provider interface too?
- What ends an interruptible wait for a frame, and what does the caller see then?

## ARCH_MAX_PFN_LIMIT is spelled ARCH (2026-11-15)

**Files**
- `kernel/arch/x86/kernel/include/ours/arch/mem_cfg.hpp` — `ARHC_MAX_PFN_LIMIT` renamed `ARCH_MAX_PFN_LIMIT`, its definition and its one use

**Symptom**
The Normal zone's bound beside `ARCH_MAX_DMA_PFN` and `ARCH_MAX_DMA32_PFN` was spelled `ARHC_MAX_PFN_LIMIT`, so a search for the arch's zone bounds by their prefix missed it.

**Cause**
A typo from the first commit.

**Change**
The rename; the value (64 GiB) is unchanged.

**Reference**
None.

**Verification**
`verify` on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed.

**Deliberately not done**
None.

**How it was found**
Noticed beside the Dma32 bound on 2026-11-14 and left for its own change.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a misspelled macro still compile and work when its one use is spelled the same way?
- What would a search for `ARCH_MAX_` have told a reader about the zone bounds before this change?
- Why is a bound like this a macro at all here, when the codebase defines constants with `constexpr`?

## The gate is shut under the watcher's lock (2026-11-15)

**Files**
- `kernel/mm/pmm.cpp` — `FreeMemoryGate::admit()` keeps its lock-free paths and hands the shut to a new `shut()`, which decides again under `m_watcher_lock` and tells the watcher of every shut; comments on the class and the lock
- `kernel/tests/memory_pressure_test.cpp` — new `MemoryPressure.TheGateIsNeverShutOnALevelNoLongerInForce`

**Symptom**
Rarely, the free-memory gate could be left shut with free memory inside the watcher's bounds. Nothing then signals the watcher, so nothing opens the gate: every allocation that can wait answers `ShouldWait`, every waiter woken by a relief is put off again, and a VMO page fault loops in `AnonymousPageRequest::allocate()` until free memory happens to leave the bounds.

**Cause**
`admit()` read the state, the delay level and the free count one after another, and then compare-and-swapped the state from `Open` to `Shut`. A compare-and-swap compares a value, not a history: the watcher's `set()` stores `Open` again when it re-arms, so a swap decided on reads made before a re-arm still found `Open` and succeeded. The gate was then shut on a delay level no longer in force. This is the ABA problem. Two smaller gaps came with it: the shutting allocation returned before `observe_pressure()`, so it never signalled the watcher itself, and `set()`'s bounds were changed under a lock the allocation side never took. The code dates from 2026-10-29; since N1 every ordinary wait ends at this gate.

**Change**
Only the rare transition is serialized:

```cpp
auto FreeMemoryGate::admit() -> Status {
    // lock-free: Shut -> ShouldWait; free >= level -> Ok; Refusing -> OutOfMem
    return shut();
}

auto FreeMemoryGate::shut() -> Status {
    {
        Guard<sync::SpinLock, IrqSave> guard{m_watcher_lock};   // the lock set() writes under
        // state, level and free again: the ones in force
        m_state.store(State::Shut, std::memory_order_release);
        armed = m_signal_event.exchange(nullptr, std::memory_order_acq_rel);
    }
    if (armed) { armed->signal(); }   // outside the lock, as every signal is
    return Status::ShouldWait;
}
```

The invariant: a shut gate means free memory was below the delay level in force when it shut, and the watcher has been told. Every write of the state is now made under `m_watcher_lock`, so `set()` cannot land between the decision and the shut, and the decision reads what `set()` last stored. The watcher is told of every shut unconditionally, by taking the armed event under the lock and signalling it after: the first version called `check_signal()` instead, which looks at the bounds again, and frees can bring free memory back inside them before it looks (at Critical the lower bound is the delay level itself), so nobody would signal and the gate would stay shut on a count the watcher would have opened it at. A watcher told of a shut it need not act on looks, finds free memory in bounds, and its `set()` opens the gate. The lock is a leaf: nothing is taken under it, and the zone walk inside reads atomics. The allocations that find the gate already shut, or free memory above the level, never take it. The alternative was a generation stored beside the state in one atomic word and bumped by `set()`, which keeps the shut lock-free but makes both sides harder to read; the shut is rare enough that a lock costs nothing measurable.

**Reference**
Zircon decides and signals in one step under the node's lock: `decrement_free_count_locked()` trips the free-pages level (`trip_free_pages_level_locked()`) and signals the memory event (`signal_free_memory_change_locked()`) with the lock held, and `set_free_memory_signal()` writes the levels under the same lock (`kernel/vm/pmm_node.rs`).

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed, on the default and the four-node machine. `ktest all`: default 91 ran, 84 passed, 7 skipped; four nodes 91 ran, 88 passed, 3 skipped; `MEM=8192` `ktest sel=MemoryPressure.*:PmNode.*:PageReclaim.*` 15 ran, 10 passed, 0 failed. Failure-injection build: 91 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. clang-tidy reports nothing in `pmm.cpp`.

The new test races a thread that admits waiting allocations as fast as it can against the main thread, which in each of 40000 rounds arms a level no free count reaches and then no level at all, waits for the admitter's allocation in flight and one more to finish, and asks with a probe confined to its node (so a zone at Low cannot answer it `ShouldWait`): with no level, only a stale shut does. It takes about six seconds. With the fix, 0 stale shuts in every run (about 118000 admissions each). Control: with the old compare-and-swap restored, the test failed in four runs of four, one to three stale shuts each; at 4000 rounds it had caught the race in only three runs of five, which is why the count is ten times that. It is a race, so a pass is evidence rather than proof; the argument above is what the fix rests on.

A narrow pre-commit review confirmed that every write of the state is under the lock, that the lock is a leaf taken only by callers that hold no spin lock, and the memory orders. It found the gap the unconditional signal closes (a high finding against the first version), that the probe could have counted a zone stopping at Low as a stale shut (hence the confined probe), and that the test could spin for ever or leave the admitter reading a dead stack if it never ran (the spin now gives up after five seconds and the join waits for the admitter to stop).

**Deliberately not done**
- `set()` still disarms and re-arms the event in steps; a `check_signal()` from an allocation that moves free memory in that window sees no event and signals nothing. The next allocation or free signals it, and a shut now signals for itself.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, H1, from the correctness reviewer's reading of `admit()` against `set()`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a compare-and-swap that succeeds not prove that what you read before it is still true, and what two cures are there?
- Why is it enough to lock only the transition from Open to Shut, and not every admission?
- What would have reopened a gate left shut this way, and why might it never come on a quiet machine?

## A Pause and the thread's take of a node record are one decision (2026-11-15)

**Files**
- `kernel/mm/include/ours/mm/memory_reclaimer.hpp`, `kernel/mm/memory_reclaimer.cpp` — new `take_node_target()`, which asks `paused()` and takes under a new spin lock `m_take_lock`; the `Pause` constructor, now out of line, takes that lock once after counting itself in; `reclaim_node_requests()` walks a taken record through a new private `walk()` under `m_reclaim_lock` without asking `paused()` again; `run()` no longer asks it; the Pause contract and two comments rewritten to match
- `kernel/tests/pm_node_test.cpp` — new `PmNode.APausedReclaimerTakesNoNodeRecord`; a drain test's comment no longer claims nothing reclaims under a Pause
- `.agents/skills/kernel-design/references/mm.md` — the rule for the take

**Symptom**
A Pause could lose node records. `run()` asked `paused()` once, then `reclaim_node_requests()` took every node's record and walked it through `reclaim()`, which asks `paused()` again and returns 0. A Pause that began between the two looks found the records taken and the walks refused: the marks were gone and no walk had run. That broke the contract N2 wrote ("while a Pause lives the thread takes no record"), and on SMP it could fail `PmNode.AShortZoneIsMarkedOnceAndTakenOnce`, since the previous test's Pause signals the thread as that test starts.

**Cause**
Check-then-act across two calls: the decision "may the thread take records now" was made by one look at `paused()`, and the action came later, with a second look that could disagree. Neither look excluded a Pause from beginning between them.

**Change**
The take and the start of a Pause are ordered by one lock, and a taken record is always walked:

```cpp
auto MemoryReclaimer::take_node_target(PmNode &node) -> usize {
    Guard<sync::SpinLock, IrqSave> guard{m_take_lock};
    if (paused()) { return 0; }                     // the record stays for later
    return node.take_reclaim_target();
}

MemoryReclaimer::Pause::Pause(MemoryReclaimer &reclaimer) : m_reclaimer(reclaimer) {
    m_reclaimer.m_pauses.fetch_add(1, std::memory_order_acq_rel);
    Guard<sync::SpinLock, IrqSave> guard{m_reclaimer.m_take_lock};   // waits out a take in progress
}

// reclaim_node_requests(), per node
auto const wanted = take_node_target(node);
if (wanted == 0) { return; }
Guard guard{m_reclaim_lock};
freed = walk(wanted, NodeMask::of(node.nid()));     // not refused by a Pause that began since
```

The argument. The take's critical section and the constructor's are excluded from each other by the lock. If the take's runs first, it took the record before the Pause took effect, and its walk runs. If the constructor's runs first, its unlock synchronizes with the take's lock, and the count it added is sequenced before that unlock, so the take sees the Pause and leaves the record. Once the constructor returns, no take starts; and no taken record is dropped, because the walk of a taken record is not refused. The lock is held for one node's take, which reads counters and never sleeps, so a Pause waits out at most that, never a walk.

So a Pause now means nothing new is reclaimed: `request()` is dropped, `reclaim()` refuses, the thread takes no record. What had already started runs to its end, as a walk past `reclaim()`'s locked check always did; the walk of a record taken just before the Pause is now one of those. The other way out was to put a record back when its walk is refused, but the put-back lands after the Pause has begun, under a test reading the record, which is what the contract exists to prevent.

**Reference**
None: the Pause is this kernel's own test seam.

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed, on the default and the four-node machine. `ktest all`: default 92 ran, 84 passed, 8 skipped; four nodes 92 ran, 89 passed, 3 skipped; `MEM=8192` `ktest sel=MemoryPressure.*:PmNode.*:PageReclaim.*` 16 ran, 10 passed, 0 failed. Failure-injection build: 92 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. clang-tidy reports nothing in `memory_reclaimer.cpp`.

The new test marks another node's zone under a Pause and asks the take for it: the record stays (a second mark is not the first); without a Pause the take clears it. Control: with the take not asking `paused()`, the test fails its Pause check. The test exercises the take's decision; the ordering against a Pause that begins mid-pass rests on the argument above, since that interleaving cannot be produced on demand.

A narrow pre-commit review worked through the orderings of the count, the constructor's lock and the take's lock and found no take after a Pause's constructor returns and no record taken and dropped; confirmed the lock is a leaf held across no sleep and that both callers of `walk()` hold `m_reclaim_lock`; and found three comments the change left stale (the report path, `m_reclaim_lock`'s holders, a drain test's "nothing reclaims"), now rewritten.

**Deliberately not done**
- A walk already under way as a Pause begins still runs, as before; a test draining a node assumes nothing marked that node short just before.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, M1, from the pressure reviewer.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why do two looks at `paused()`, one before the take and one before the walk, still let a Pause lose a record?
- Why must the Pause constructor take `m_take_lock` after its `fetch_add`, and not before?
- Why walk a record taken just before a Pause instead of putting it back?
