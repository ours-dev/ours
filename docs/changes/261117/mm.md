# mm changes

## The PMM tests keep their own state in scope objects and check what their names say (2026-11-17)

**Files**
- `kernel/tests/pm_node_test.cpp` — `find_counter()` and `count_around()` replace two hand-written counter lookups and interrupts-off read-arounds; the allocation `check_local_fallback()` makes with interrupts off asks `Gaf{}` instead of `ALLOC_KERNEL`, which says it may block; the exhaust-remote check is its own test, `FallsBackFromAnExhaustedRemoteNode`; `WaitingRequestsStopAtLowAndWakeOnRelief` becomes `WaitingRequestsStopAtLowAndReliefIsCountedOnce`; `check_node_queries()` counts nodes against `find_node()` instead of comparing `num_nodes()` with itself, and the absent-node half is `AbsentNodesAnswerNothing`; `AllocationsTakeTheHighestZoneFirst` expects a zone above its own Low floor; `AllocationsDisabled` scopes `AllocState::disable()`; `std::to_underlying`, operand order, and no `return` after `FAIL`
- `kernel/tests/memory_pressure_test.cpp` — `GateRestore` opens the gate on every way out of a check that shut it; `spawn_running()` replaces three spawn-and-resume copies, two of which dropped `resume()`'s status
- `kernel/mm/tests/smoke_checks.hpp` (new), `kernel/mm/tests/admission_smoke.cpp`, `kernel/mm/tests/alloc_request_smoke.cpp` — the two host smoke tests share a `SmokeChecks` tally instead of each keeping a file-scope counter; output unchanged; their file comments say they run every check
- `kernel/mm/pm_node.cpp` — `connect()`'s comment no longer names `m_connected`, a member an earlier commit removed

**Symptom**
Latent, from the review's minor list. One test was skipped whole because one optional part of it skipped; one compared a value with itself; one named a wake no thread waited for; one picked its expected zone by a margin the allocator does not use and could fail under load; and several tests undid the gate or the allocation switch by hand on each exit, so a `FAIL` added on a new path would have left the gate shut, or pointing at a stack `Event` that no longer exists, for every test after it.

**Cause**
The same lesson as `ZoneTrial` (review M11): state a test changes for its own run is undone by a scope object, not by a line on each exit. And a test's name and messages are what a reader of a failure sees first: when they say more than the test checks, the failure points at the wrong thing.

**Change**

```cpp
class GateRestore {             // memory_pressure_test.cpp
  public:
    GateRestore() = default;
    ~GateRestore() { open_gate(); }
    ...
};
// declared after the stack Events in check_signal(): destroyed first, so the
// signal is disarmed before the Event it points at goes

auto const nodes = /* ids find_node() answers for */;
EXPECT_TRUE(nodes == mm::num_nodes(), ...);   // was num_nodes() == num_nodes()
```

**Reference**
None: test structure.

**Verification**
On the tree that holds this and the two batches before it (the watchdog's declaration, the node's CPU bookkeeping), in the `feat/pmm-redesign` worktree: `verify` on the default and the four-node machine, dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed with the smoke tests' output unchanged. `ktest all`: default 99 ran, 84 passed, 15 skipped; four nodes 99 ran, 95 passed, 4 skipped (the exhaust-remote test without its option among them); failure-injection build 99 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. clang-tidy reports only the old naming warning in `pm_node_test.cpp`. Not run: `FallsBackFromAnExhaustedRemoteNode` with the exhaust-remote option, which needs a guest with a memory-only node. A narrow pre-commit review of the three batches confirmed no finding; it noted the stale `m_connected` comment (fixed) and that the exhaust-remote path is not run (said above).

**Deliberately not done**
Reaching the reserve check through the node walk instead of the zone (minor item 53), and driving draining tests above the seam (42): both depend on whether the node and zone layers stay public, a question put to the user.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor items 4, 43, 44, 45, 46, 47, 48, 50, 51 and 55.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must `GateRestore` be declared after the stack `Event`s in `check_signal()`?
- Why did a `KTEST_SKIP` in a helper turn the whole composite test into a skip?
- What could `AllocationsTakeTheHighestZoneFirst` expect wrongly with a bare margin, and why does the zone's own floor fix it?
