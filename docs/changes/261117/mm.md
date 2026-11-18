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

## A waiting request is put off at Low only when something is bound to end its wait (2026-11-17)

**Files**
- `kernel/mm/pmm.cpp` — `FreeMemoryGate::ends_waits()`: the watchdog has armed the gate; `AllocationRun::take()` puts a request off at Low only when it does
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` — `waits_at_low()` also requires that the request allow no zone above Normal
- `kernel/mm/tests/alloc_request_smoke.cpp`, `kernel/tests/pm_node_test.cpp` — a request allowing Movable does not wait at Low; `AWaitingRequestGoesOnToMinWithNoWatchdog`

**Symptom**
Latent. With no watchdog armed, which is the case before its init hook runs and for good when the `kernel.oom` levels do not nest (review M10 refuses them and starts none), the gate's delay level is zero and it admits everything; a waiting request put off at a zone's Low then waits on the gate's event, which only a release crossing a zone's Low signals. If nothing is released, and nothing reclaimable is left, it waits for ever: no watchdog declares OutOfMemory, so nothing refuses it. Separately, a request allowing zones above Normal has higher floors in the zones below (their reserve against it), which the watchdog's levels do not cover, so the same wait could hang there; no `Gaf` bit makes such a request today.

**Cause**
"A request put off at Low is woken" was a property of the whole system (watchdog started, levels covering the floors, only Normal-bounded requests), stated in DESIGN.md and assumed by `alloc_block()`, not checked where the decision to wait is made. The decision to put a request off has to ask whether anything can end the wait, the same question that already sends a confined request on to Min.

**Change**

```cpp
// FreeMemoryGate
auto ends_waits() const -> bool { return m_delay_level.load(std::memory_order_relaxed) != 0; }

// AllocationRun::take(), after a Low pass that found no zone
if (request.waits_at_low() && GATE.ends_waits()) { return ustl::err(Status::ShouldWait); }
block = start->alloc_frames(request, AdmissionPass::Min);

// AllocRequest
return may_wait() && !is_confined() && m_highest_zone <= ZoneType::Normal;   // waits_at_low()
```

A request that is not put off makes the Min pass, as one that cannot wait does, and fails there with `OutOfMem` if the zones are below Min too: an answer instead of a wait. A relaxed read is enough: a waiter that reads zero just as the watchdog arms the gate goes on to Min once more, which is what every request did before the gate existed.

**Reference**
None: Linux does not put allocations off on a gate; its waits end through `__alloc_pages_slowpath()`'s retry loop and the OOM killer.

**Verification**
As for the change before. Control: with `take()` asking `waits_at_low()` alone, `AWaitingRequestGoesOnToMinWithNoWatchdog` fails (the request is answered `ShouldWait`). The narrow pre-commit review found that `WaitingRequestsStopAtLowAndReliefIsCountedOnce` now relied on a gate the watchdog had armed without arming it itself, so it would fail with no watchdog, or in a shuffled run right after a test that left the gate unarmed (it holds the watchdog and arms the gate by hand now), and that `alloc_frames()`'s documentation in `pmm.hpp` did not give the new conditions (it does now). Refuted: that the confined and bound waiting-request tests pass vacuously while the gate is unarmed (the gate is armed when they run, and confinement alone sends them to Min), and that the comment on `ends_waits()` overstates the case before the watchdog starts.

**Deliberately not done**
Falling back to default levels when the command line's do not nest (option B): it would undo M10's "refused and reported, not repaired".

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 36, decided by the user on 2026-11-17 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- What ends the wait of a request put off at Low, and what is missing when no watchdog runs?
- Why is a relaxed read of the delay level enough here?
- Why does a request allowing Movable not wait at Low, while one allowing up to Normal does?

## The node and zone layers under the pipeline take a passkey (2026-11-17)

**Files**
- `kernel/mm/include/ours/mm/priv/layer_keys.hpp` (new) — `NodeLayerKey` (made by `priv::Pipeline` and `test::LayerKeys`) and `ZoneLayerKey` (made by `PmNode`, `priv::Pipeline` and `test::LayerKeys`)
- `kernel/mm/include/ours/mm/test/layer_keys.hpp` (new) — `test::LayerKeys`, the kernel tests' door
- `kernel/mm/include/ours/mm/pm_node.hpp`, `kernel/mm/pm_node.cpp`, `kernel/mm/pm_node_init.cpp` — `PmNode::alloc_frames()` and `free_frames()` take a `NodeLayerKey`; the node makes a `ZoneLayerKey` for its walk and for bring-up's watermark setup
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `PmZone::alloc_frames()`, `free_frames()`, `adopt()`, `setup_watermarks()` and `setup_lowmem_reserve()` take a `ZoneLayerKey`; `friend class PmNode` is gone, which showed `m_flags` is read by nothing (kept, marked)
- `kernel/mm/pmm.cpp` — `priv::Pipeline`, the pipeline's door, and its keys at the four calls down
- `kernel/tests/pm_zone_test.cpp`, `kernel/tests/pm_node_test.cpp`, `kernel/tests/page_reclaim_test.cpp` — through the test door; `LowZonesKeepAReserve` also checks the reserve through the node's walk
- `.agents/skills/kernel-design/references/mm.md`, `.agents/tasks/pmm-redesign/DESIGN.md` — the layers' access as built (the reference said "private and reached through friend", which they never were)

**Symptom**
Latent. The node's and the zone's `alloc_frames()` and `free_frames()` were public, so any caller could go under the pipeline: a block freed straight into a zone skipped `prepare_free()`'s checks (reserved, pinned, not a head), `clear_block()`, the relief wake a waiter at Low depends on, and the pressure observation; a block taken straight from a zone skipped the thread's allocation switch, every floor, zeroing and the counters. Only `pmm.cpp` and the tests did it, by convention.

**Cause**
"Only the pipeline calls the layers under it" is an invariant over callers, and a public member states the opposite. A friend would state it, but opens every private member of the class to the friend; a passkey, a type only the intended caller can construct, taken as a parameter, opens one function to one caller (review T5). Tests legitimately need to go under: draining a zone below every floor is what the pipeline will not do, so they get a door of their own, named for what it is.

**Change**

```cpp
class NodeLayerKey {                         // priv/layer_keys.hpp
    friend class priv::Pipeline;             // defined only in pmm.cpp
    friend class test::LayerKeys;            // ours/mm/test/layer_keys.hpp
    NodeLayerKey() = default;
};
auto PmNode::alloc_frames(AllocRequest const &, AdmissionPass, NodeLayerKey) -> ...;
auto PmZone::free_frames(PmFrame &, ZoneLayerKey) -> bool;

// pmm.cpp
auto block = start->alloc_frames(request, AdmissionPass::Low, priv::Pipeline::node_key());
// a test
auto block = zone.alloc_frames(order, mm::test::LayerKeys::zone());
```

What the keys do not prevent, said in their comments: another file could define its own `priv::Pipeline`, and any file can include the test door. They guard against reaching the layers by mistake, which is the risk, not on purpose. `LowZonesKeepAReserve` now also walks the node with a request that allows nothing above the low zone, which shows the walk asks each zone with the request's own highest type; its refusal half stays a question to the zone, since a request that allows Normal would be served by the zones above.

**Reference**
None for the keys: a C++ idiom (the passkey). Linux keeps its zone allocator's internals (`rmqueue()`, `__free_one_page()`) `static` to `mm/page_alloc.c`, file scope doing what the keys do across this kernel's files.

**Verification**
`verify` in the `feat/pmm-redesign` worktree on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed. `ktest all`: default 100 ran, 84 passed, 16 skipped; four nodes 100 ran, 96 passed, 4 skipped, `LowZonesKeepAReserve` with its new walk check among the passes; failure-injection build 100 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. clang-tidy reports nothing in `pmm.cpp`, `pm_zone.cpp` and `pm_node.cpp`. Control: a test that builds `mm::ZoneLayerKey{}` itself fails to compile ("calling a private constructor of class `ZoneLayerKey`").

A narrow pre-commit review (two reviewers, each finding checked by a second agent; the first attempt lost both to dropped connections and was run again) found two blank lines left where the friend block was removed (one deleted), `PmNode::free_frames()`'s comment putting the waiter's wake before the release when it comes after and depends on its result (corrected), and the new classes not declared `final` (they are now). Refuted: that a public `PmNode::setup_watermarks()` makes the zone's keyed setup pointless (it was the caller before and is still the only one), that another file defining `priv::Pipeline` breaks the seal (said in the comments, and an ODR violation besides), and that the design document's older "target" sentence must go (it keeps its history with a dated note, as the rest of it does).

**Deliberately not done**
Sealing `PmZone::admits()`, a query with no effect. Removing `m_flags`, a stub for state the zone will need.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor items 25, 40, 42 and 53, decided by the user on 2026-11-17 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does a block freed straight into a zone skip, and which of those can leave a thread waiting for ever?
- What does a passkey open that a friend does not, and what does neither stop?
- Why do tests need a door under the pipeline at all?

## A frame keeps every rule of its pins with the count (2026-11-17)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `PageFrame::pin()` and `unpin()` return `Status` and hold every rule: the head of a free block (`Role::Pmm`) is refused, a full count (255) is refused, an unpin of none is refused
- `kernel/mm/pmm.cpp` — `mm::pin_frame()` and `unpin_frame()` forward; their documentation keeps the caller's side of the contract (the owner's lock)

**Symptom**
Latent, from the review's minor list. The rule "a free block's head is pinned by nobody" was checked by `mm::pin_frame()`, the count's limit by `PageFrame::pin()`. `pin()` is public, so a caller going straight to the frame skipped the first rule.

**Cause**
The frame knows its own role, so "a free frame cannot be pinned" is a fact about the frame, like the limit beside it. Split between the seam and the frame, each half was right and the whole was kept by convention.

**Change**

```cpp
auto PageFrame::pin() -> Status {
    if (is_role(Role::Pmm)) { return Status::BadState; }
    if (m_pins.load(std::memory_order_relaxed) == std::numeric_limits<u8>::max()) { return Status::NoResource; }
    ...
    return Status::Ok;
}
auto pin_frame(PmFrame *frame) -> Status { return frame->pin(); }   // the contract lives here
```

The same answers in the same order as before, so callers see no change.

**Reference**
None: the pin count is this kernel's own (F1 of the redesign).

**Verification**
Recorded with the next change, on the tree that holds both.

**Deliberately not done**
Removing `mm::pin_frame()`: it is where the caller's half of the contract, the owner's lock, is written down.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 7, decided by the user on 2026-11-17 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the free-frame rule belong to the frame rather than to the seam?
- What does `mm::pin_frame()` still say that the frame cannot?
- Why can the read and the write of the pin count be two steps?

## The reserved books are wired and unwired by the zone, flag and count together (2026-11-17)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `PmZone::wire(PmFrame &)` marks a frame Reserved and counts it; `unwire(PmFrame &)` clears it and uncounts it; `add_reserved_frames()` and `sub_reserved_frames()` are gone
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `WireKey`; `mark_reserved()` and `clear_reserved()` take it (made by `PmZone`, `priv::MemmapInit` and `priv::Pipeline`)
- `kernel/mm/memmap_init.cpp` — `priv::MemmapInit`, bring-up's door for holes and for frames with no zone to count them in; `stamp_reserved_frames()` wires through the zone
- `kernel/mm/pmm.cpp` — `mm::unwire_frame()` unwires through the zone; `priv::Pipeline::wire_key()` for a frame no zone counted

**Symptom**
Latent, from the review's minor list. A reserved frame is two records: the frame's Reserved flag and one in its zone's reserved count, from which `managed_frames()`, and so every watermark and low-zone reserve, is worked out. Bring-up wrote them with two calls (`mark_reserved()`, then `add_reserved_frames(1)`), and `mm::unwire_frame()` cleared them with two (`clear_reserved()`, then `sub_reserved_frames(1)`). A caller that wrote one and not the other would leave the count and the flags disagreeing, and the watermarks wrong, with nothing to notice.

**Cause**
One fact kept in two places needs one owner that writes both. The zone holds the count and knows its frames, so it is that owner; the frame's flag setters take a key so that only the zone, and the two places that mark a frame no zone counts, can write the flag alone. The names are the user's (2026-11-17): `wire()` and `unwire()`, the pair `mm::unwire_frame()` already began, and the word for boot-reserved pages in Zircon's `boot_reserve_wire()`.

**Change**

```cpp
auto PmZone::wire(PmFrame &frame) -> void {          // bring-up
    frame.mark_reserved(WireKey{});
    m_reserved_frames += 1;
}
auto PmZone::unwire(PmFrame &frame) -> void {        // mm::unwire_frame()
    frame.clear_reserved(WireKey{});
    m_reserved_frames -= 1;
}

// stamp_reserved_frames(): through the zone where there is one
if (zone != nullptr) { zone->wire(*frame); } else { frame->mark_reserved(priv::MemmapInit::wire_key()); }
```

Every frame is marked and counted exactly as before: a hole is marked and never counted, a reservation on a node with no zone of its type is marked and not counted, and the rest are both.

**Reference**
Zircon `boot_reserve_wire()` (`kernel/vm/bootreserve.cc`) marks the boot-reserved ranges' pages wired; Linux keeps the equivalent as `PageReserved()` on the page and `zone->managed_pages` counted apart (`adjust_managed_page_count()`, `free_reserved_page()`), the two written together by its helpers.

**Verification**
On the tree that holds this and the change before, in the `feat/pmm-redesign` worktree: `verify` on the default and the four-node machine, dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed. `ktest all`: default 100 ran, 84 passed, 16 skipped; four nodes 100 ran, 96 passed, 4 skipped; failure-injection build 100 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. At 8 GiB the zones' floors hold 14682 frames, as before this change, so every zone's managed count, which the floors are worked out from, is unchanged. clang-tidy reports nothing in `memmap_init.cpp` or `pmm.cpp`. A narrow pre-commit review found the comments of `mark_reserved()` and `clear_reserved()` naming only holes as marked but uncounted and only the zone as the way off the books (both now name the zone-less reservation too); refuted: that `wire()` and `unwire()` should take a key themselves (they keep the flag and the count together, which is the invariant; who calls them is bring-up and `mm::unwire_frame()` alone, and nothing outside could desynchronise the books through them).

**Deliberately not done**
Renaming the flag and the count to "wired": the fact is "held since boot", which `Reserved` names; `wire` and `unwire` are the acts of putting a frame on and off those books.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor items 8 and 13, decided by the user on 2026-11-17 (option A, named by the user).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which two records make a frame reserved, and what goes wrong if they disagree?
- Why is the zone the owner, and why do two other places still hold a key to the flag?
- Why do `wire()` and `unwire()` not need a key of their own?
