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

## The CPU-to-node table and the per-CPU node slot are read and written whole (2026-11-17)

**Files**
- `kernel/mm/include/ours/mm/cpu_node_map.hpp`, `kernel/mm/cpu_node_map.cpp` — `node_of()` and `set_node_of()` go through `std::atomic_ref<NodeId>`, relaxed; `cache_local_node()` writes another CPU's slot through `std::atomic_ref<PmNode *>`, relaxed; the table's comment says who reads and writes it and why relaxed is enough

**Symptom**
Latent: `move()` has no caller yet. Once it has one, it writes a CPU's table entry and per-CPU slot from another CPU while any CPU may be reading them with no lock, as plain memory: a data race, which the C++ memory model makes undefined however harmless a word-sized store looks on the machine.

**Cause**
The objects: the table `NODE_OF_CPU`, one `NodeId` per CPU; each CPU's slot, a `PmNode *` copy of `find_node(table[cpu])`. The readers: `current_node()` and `local_node()` on any CPU, with no lock. The writers: the seed at bring-up, `publish()`, and `move()` from another CPU. The worst interleaving: a reader loads the entry while `move()` stores it. The invariant each reader needs is only "a node that exists, or the not-placed sentinel"; nothing is ordered after the read, since an allocation may start on any node. So the fix is atomicity, not ordering: each word read and written whole, relaxed.

**Change**

```cpp
return std::atomic_ref<NodeId>{NODE_OF_CPU[cpu]}.load(std::memory_order_relaxed);        // node_of()
std::atomic_ref<NodeId>{NODE_OF_CPU[cpu]}.store(nid, std::memory_order_relaxed);         // set_node_of()
std::atomic_ref<PmNode *>{Slot::get(cpu)}.store(node, std::memory_order_relaxed);        // cache_local_node()
```

The slot's own reader is a `%gs`-relative load in inline assembly, one instruction, so it is not a C++ access the model could tear; the remote writer is the one that needed to change. On x86 a relaxed load or store of a word is the plain `mov` it was, so nothing is slower.

**Reference**
Linux keeps the CPU-to-node map as a per-CPU `numa_node`, read by `cpu_to_node()` and written by `set_cpu_numa_node()` (`include/linux/topology.h`) as plain single-word accesses: the kernel's own memory model counts an aligned word access as whole, which C++'s does not, so here the same accesses are spelled as relaxed atomics.

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed. Not exercised: `move()` has no caller, so no run reaches the race this closes.

**Deliberately not done**
Ordering the table, the slot and the two nodes' masks as one transaction: a reader that sees them out of step for an instant gets a valid answer, as the comment on `move()` already says of the masks.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 39, decided by the user on 2026-11-17 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which readers and writers race on the table, and what is the worst interleaving?
- Why is relaxed enough here, and what would need acquire and release?
- Why did the slot's local read not need to change?

## Page tables are asked for as a caller that cannot block (2026-11-17)

**Files**
- `kernel/mm/pmm.cpp` — `alloc_page_table()` asks `Gaf::Zero` instead of `ALLOC_KERNEL | Gaf::Zero`, and says why

**Symptom**
`alloc_page_table()` carried `Gaf::CanBlock` while both of its callers, `PageTable::allocate_table()` and `PageTableDeposits::reserve()`, run under a page table's spin lock with interrupts off. The flag did not make it sleep; it picked the floor of the Min pass, so page tables stopped at Min while every other caller that cannot block may go a quarter below it.

**Cause**
`Gaf::CanBlock` describes the caller's context (Z3 of the redesign): a caller that may block is stopped first, at Min, and one that may not keeps the band below it. A flag that says the opposite of the context gives the request the wrong floor, quietly.

**Change**

```cpp
// before
priv::alloc_frames(ALLOC_KERNEL | Gaf::Zero, get_order(size / PAGE_SIZE), node, node_online_mask());
// after: every caller holds a page table's spin lock with interrupts off
priv::alloc_frames(Gaf::Zero, get_order(size / PAGE_SIZE), node, node_online_mask());
```

Page-table allocations may now take a zone down to three quarters of Min, as atomic callers do: a page table is what lets the mapping that needs it go on.

**Reference**
Linux allocates page-table pages before it takes the page-table lock (`__pte_alloc()` allocates, then takes `pmd_lock()` and checks whether another thread populated it first), so its page tables can sleep (`GFP_PGTABLE_KERNEL`). Doing the same here is option B, filed below.

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed. The `ktest all` runs on every shape are recorded with the last change of this group.

**Deliberately not done**
Allocating page-table pages outside the lock, so that they could block (option B): a change to the architecture's page-table code, not chosen for now.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 37, decided by the user on 2026-11-17 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does `Gaf::CanBlock` change for a request, and what does it not?
- Why did page tables stop at Min before, and why may they go below it now?
- What would allocating page tables outside the lock buy, and what does it cost?

## A watchdog runs only with levels that cover the zones' floors, by type (2026-11-17)

**Files**
- `kernel/mm/include/ours/mm/memory_watchdog.hpp`, `kernel/mm/memory_watchdog.cpp` — `CoveredLevels`, made only by `MemoryPressureLevels::cover_zone_floors()`, which reads `zone_floor_frame_count()` itself; `MemoryWatchdog::start()` and `set_levels()` take it; the init hook covers through it and logs the raise from it
- `kernel/tests/memory_pressure_test.cpp`, `kernel/tests/page_reclaim_test.cpp` — the levels the tests move to, and the ones they put back, go through `cover_zone_floors()`

**Symptom**
Latent. Rule D1(a), the delay level above every frame the zones' floors can hold, is what makes the gate end every ordinary wait (a zone refusing at Low then implies free memory below the delay level). It was applied by the init hook alone: `start()` and `set_levels()` took any levels that nest, so a test, or a later caller, could run the watchdog with levels that break it, and nothing would say so.

**Cause**
A rule kept by one caller of an interface is a convention; a rule the interface's parameter type carries is kept by every caller. The same move as review M10, which made "the levels nest" a property of `MemoryPressureLevels`: here "the levels cover the floors" is a property of `CoveredLevels`, and the only way to make one reads the floors.

**Change**

```cpp
class CoveredLevels {                 // made only by cover_zone_floors()
  public:
    auto levels() const -> MemoryPressureLevels const &;
    auto held() const -> usize;       // the floors it was covered against
  private:
    friend class MemoryPressureLevels;
    CoveredLevels(MemoryPressureLevels const &levels, usize held);
    ...
};
auto MemoryPressureLevels::cover_zone_floors() const -> CoveredLevels {
    auto const held = zone_floor_frame_count();
    return CoveredLevels{covering(held), held};
}
auto MemoryWatchdog::start(CoveredLevels const &levels) -> Status;
auto MemoryWatchdog::set_levels(CoveredLevels const &levels) -> void;
```

`covering(held)` stays, pure, for the test that checks its arithmetic with made-up floors. The tests' levels sit near the free memory of the moment, far above the floors, so covering leaves them as they were.

**Reference**
None: this kernel's own watchdog rule (D1(a), 2026-11-14).

**Verification**
On the tree that holds this and the next change, in the `feat/pmm-redesign` worktree: `verify` on the default and the four-node machine, dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed. `ktest all`: default 100 ran, 84 passed, 16 skipped; four nodes 100 ran, 96 passed, 4 skipped; failure-injection build 100 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. At 8 GiB the pressure and reclaim suites pass, and the levels in force cover the floors (14682 frames held, OutOfMemory at 14939). Control: a test that hands `set_levels()` raw levels fails to compile ("no viable conversion from `MemoryPressureLevels` to `CoveredLevels`"). A narrow pre-commit review found that the forward declaration of `CoveredLevels` had been put between `MemoryPressureLevels` and its doc comment, which then documented the wrong declaration (moved), and that this record was not yet written; refuted: that the design reference's D1(a) bullet had gone stale.

**Deliberately not done**
None.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 28, decided by the user on 2026-11-17 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does D1(a) guarantee, and which wait could hang without it?
- Why does `cover_zone_floors()` read the floors itself rather than take them as an argument?
- Why do the tests' levels come through unchanged?
