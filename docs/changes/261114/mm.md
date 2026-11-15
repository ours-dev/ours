# mm changes

## CPU membership leaves PmNode for CpuNodeMap (2026-11-14)

**Files**
- `kernel/mm/include/ours/mm/cpu_node_map.hpp`, `kernel/mm/cpu_node_map.cpp` — `CpuNodeMap::publish()`, `publish_all()`, `claim_possible()` and `move()`, and the two init hooks that run the publish and the claim
- `kernel/mm/include/ours/mm/pm_node.hpp`, `kernel/mm/pm_node.cpp` — `PmNode::publish_cpu()`, `publish_cpus()`, `claim_possible_cpus()` and `set_cpu_node()` go; `add_cpu()`, `remove_cpu()` and `join_hard_affinity()` are the node's own part; five includes `pm_node.cpp` no longer needs go
- `kernel/mm/pm_node_init.cpp`, `kernel/mm/pmm.cpp`, `kernel/arch/x86/mm/srat.cpp`, `kernel/lib/gktl/include/gktl/init_hook.hpp`, `kernel/include/ours/task/scheduler_domains.hpp`, `kernel/kernel/task/scheduler_domains.cpp` — mentions of the old names

**Symptom**
None. CPUs are placed, published and claimed as before.

**Cause**
Which node a CPU works on has three records: `CpuNodeMap`'s table, the CPU's per-CPU cache of its node pointer, and each node's mask of the CPUs working on it. `CpuNodeMap` owned the first two and `PmNode` the third, and the functions that keep all three in step (publish a CPU at boot, claim the CPUs the MADT found, move a CPU between nodes) were static members of `PmNode`, reaching into `CpuNodeMap` for the table and the cache and into two nodes' private members for their masks.

The operation that spans the three records belongs with the record that is the truth, the table, and each node should do only its own part. A static function on `PmNode` that writes another node's mask under that node's lock is a node doing another node's work, which is what made `PmNode` "all nodes" as well as "one node".

**Change**
`CpuNodeMap` holds the operations; a node exposes the two things only it can do to its own state:

```cpp
// before: pm_node.cpp, a static function reaching into two nodes
Guard<sync::SpinLock, IrqSave> guard{node->get_lock()};
node->m_work_cpus.set(cpu, 0);
...
CpuNodeMap::set_node_of(cpu, new_nid);

// after: cpu_node_map.cpp
node->remove_cpu(cpu);          // takes that node's lock for that node's mask
set_node_of(cpu, new_nid);
...
node->add_cpu(cpu);
cache_local_node(cpu, node);
```

The hard affinity gets a member of its own, `join_hard_affinity()`, and is not folded into `add_cpu()`. The two masks have different rules. The work mask changes at run time and is read through a copy taken under the node's lock. The hard affinity is read without a lock for the rest of the system's life, so it may be written only while the boot CPU is the only one running: in `init()`, and from the two init hooks, which run before `x86_bring_aps()` at `InitLevel::Platform`. `move()` must never touch it, and keeping it out of `add_cpu()` is what keeps it out of `move()`.

Today `join_hard_affinity()` adds nothing: the SRAT parse fills the firmware's table at `PrePmm + 1`, before any node exists, and `init()` already walks every CPU of it. The old `publish_cpu()` made the same redundant write; it stays so that this step changes nothing.

The review of this step found two comments false that it had carried over, and they are corrected here rather than moved as they were: the hard affinity's comment said the per-CPU allocator keeps a pointer to the mask for the life of the system, and nothing does (the per-CPU unit mapper reads it once, during `CpuLocal::init()`); and the publish hook's comment said a hook at exactly `CpuLocal` would run before `main.cpp` announced the level, while one at `CpuLocal + 1` would not. `set_init_level(CpuLocal)` runs every hook from `CpuLocal` up to the next level in one call, before it records the level, so the two run together and the `+ 1` only orders them.

**Reference**
Linux `numa_set_node()`, `set_cpu_numa_node()` and `node_to_cpumask_map[]` in `arch/x86/mm/numa.c`: the CPU-to-node table and the per-node CPU masks are kept by the arch's NUMA code, not by `pg_data_t`.

**Verification**
`verify` in the `feat/pmm-redesign` worktree, counts unchanged: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 183 passed, on the default and the four-node (`NUMA=1`) machine. `ktest all` on dbg, both machines: 83 ran, 80 passed, 3 skipped. After the three comment corrections, `verify` on the default machine again, the same. `format_cpp.py` clean; clang-tidy reports nothing in `cpu_node_map.cpp`, `pm_node.cpp` or `srat.cpp` (the last against the NUMA build), and in `pm_node.hpp` only the `[[nodiscard]]` note on `ZoneList::fallback()` this step does not touch.

A review of this step and of the node-table step, run as three narrow reviewers one after another: statement-by-statement equivalence with the old functions (seven points, all pass), the node table (five points, all pass, including that no caller of `find_node()` can reach its new bounds assertion), and the truth of every comment this step added. Three comment findings, each checked against the code; two carried over, one new, all corrected.

**Deliberately not done**
- `PmNode::bootstrap()`, `setup_watermarks()` and `create_nodes()`, bring-up of every node at once, stay static members of `PmNode`.
- `CpuNodeMap::move()` still has no caller, and the scheduler's domains do not follow a move (`docs/repairs/261103/kernel-task-sched.md`).
- `join_hard_affinity()` is kept although it adds nothing today; see Change.

**How it was found**
None. The CPU membership of `.agents/tasks/pmm-redesign/DESIGN.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the hard affinity written by its own member and not by `add_cpu()`, and what would go wrong if `move()` wrote it?
- Which of the three records of a CPU's node is the truth, and why does that decide where `move()` lives?
- `set_init_level(CpuLocal)` runs hooks at `CpuLocal` and `CpuLocal + 1` in one call. What, then, does the `+ 1` buy?

## An allocation tries a node's highest zone first (2026-11-14)

**Files**
- `kernel/mm/include/ours/mm/pm_node.hpp` — `ZoneList::push_zones_of()` appends one node's zones from the highest type down; `take_local_zones()` uses it; the class comment says the order
- `kernel/mm/pm_node.cpp` — `ZoneList::connect()` uses it for every remote node
- `kernel/tests/pm_node_test.cpp` — the zone-list check expects a node's own zones highest first and every node's zones as one falling run; new `PmNode.AllocationsTakeTheHighestZoneFirst`
- `docs/repairs/261028/mm.md` — item 1 removed

**Symptom**
Every ordinary allocation took frames from the Dma zone first. On the default 512 MiB machine that zone is 3388 frames, below 16 MiB, and the only memory a device limited to 24-bit addresses can use; ordinary kernel and user allocations spent it from the first allocation on, while the Dma32 zone's 122515 frames waited. No caller can ask for a low zone yet, so nothing failed; the first that could would have found the memory gone.

**Cause**
The fallback order an allocation walks was built in zone-type order, and `ZoneType::Dma` is 0. `take_local_zones()` and `connect()` both appended a node's zones through `for_each_local_zone()`, which walks the type table from 0 up. The walk takes from the first zone that has a block, so ascending order meant lowest first.

The zones are not interchangeable in both directions. A frame from any zone serves an ordinary request; only a frame from a low zone serves a request with an address limit. Serving ordinary requests from the low zones first spends the one supply that has no substitute while the one that has a substitute sits idle, which is backwards from any allocator that has to serve both.

`for_each_local_zone()` itself was not wrong: it is a walk over a node's zones by type, and memmap initialisation, the reclaimer and several tests use it that way. The mistake was reusing it to mean allocation order.

**Change**
The fallback order is built by a walk of its own, from the highest type down:

```cpp
// pm_node.hpp, ZoneList
auto push_zones_of(NodeId const nid) -> void {
    auto const row = PmZone::row(nid);
    for (auto type = row.size(); type > 0; --type) {
        if (auto *const zone = row[type - 1]) {
            push_fallback(zone);
        }
    }
}
```

`take_local_zones()` calls it for the node itself and `connect()` for each remote node in the order the NUMA layer derived. The result is node first, zone second: on four nodes, node 0's list is `0:Dma32, 0:Dma, 1:Dma32, 1:Dma, ...`. A node's own low zones are still tried before another node's high zones, because a remote frame costs every access to it for as long as it lives, and a low zone spent costs only a later constrained request. The other arrangement, every node's Normal before any node's Dma32, keeps the low zones and gives up locality for it (decided with the user, 2026-11-14).

`for_each_local_zone()` keeps walking by type, for its other users.

**Reference**
Linux `build_zonelists()` and `build_zonerefs_node()` in `mm/page_alloc.c`, which fill each node's zonelist node by node, and within a node from `MAX_NR_ZONES - 1` down. Linux had a zone-ordered zonelist as an option (`numa_zonelist_order=zone`) and removed it in 4.14 for the locality reason above.

**Verification**
`verify` in the `feat/pmm-redesign` worktree, counts unchanged: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 183 passed, on the default and the four-node machine. `ktest all` on dbg, both machines: 84 ran, 81 passed, 3 skipped; one more than before, the new test. With `MEM=3072`, where a Normal zone of 640150 frames exists above the Dma32 zone, both images boot and `ktest PmNode.*:PmZone.*` passes, the new test taking its frame from Normal.

Control: with `push_zones_of()` walking upwards again, the new test fails (the frame comes from Dma), and so do both new checks of the zone list (`head_ok` and `runs_ok`). The fix restored, all pass.

`format_cpp.py` clean; clang-tidy reports nothing in `pm_node.cpp`, and in `pm_node.hpp` only the `[[nodiscard]]` note on `ZoneList::fallback()`, untouched here.

**Deliberately not done**
The low-zone reserve, Linux's `lowmem_reserve[]`: a share of each low zone that a request able to use a higher zone may not take. Ordering alone still lets ordinary allocations reach the low zones once every higher zone is short. The reserve is an admission rule, and nothing on the allocation path reads a watermark yet; it is built with the watermark floors (decision Z3 of `.agents/tasks/pmm-redesign/DESIGN.md`), where it has a reader.

**How it was found**
Recorded on 2026-10-28 as item 1 of `docs/repairs/261028/mm.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is "lowest zone first" backwards, when every zone's frames are equally good for an ordinary request?
- Node 0's Dma zone or node 1's Dma32 zone: which does an allocation on node 0 try first, and what is given up by the other choice?
- What can still drain a low zone after this change, and what is the mechanism that stops it?

## Allocation reads the watermarks: two passes, Low then Min (2026-11-14)

**Files**
- `kernel/mm/include/ours/mm/priv/admission.hpp` — new: `AdmissionPass`, `admission_floor()` and `admits()`, the arithmetic with every input an argument
- `kernel/mm/tests/admission_smoke.cpp`, `kernel/mm/tests/BUILD.gn` — new host test of it
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `PmZone::admits(order, pass, can_block)`; `watermark_ok()` uses the same arithmetic
- `kernel/mm/include/ours/mm/pm_node.hpp`, `kernel/mm/pm_node.cpp` — `PmNode::alloc_frames(request, pass)` passes over a zone that does not admit the request and names the first as short
- `kernel/mm/pmm.cpp` — `alloc_block()` makes the two passes and reports the short zone to every allocation form, on success as well as failure
- `kernel/mm/include/ours/mm/alloc_flags.hpp` — `Gaf::CanBlock` has a reader; `ALLOC_USER` carries it
- `kernel/tests/pm_node_test.cpp` — new `PmNode.AdmissionHoldsAtLowThenAtMin`; the exhausted-node check drains past the floor through the zones themselves
- `kernel/tests/page_reclaim_test.cpp` — the targeted-reclaim check takes its resupplied frame from the zone

**Symptom**
Every zone's three watermarks were computed at boot and read by nothing on the allocation path. An allocation took frames from a zone until its buddy was empty: the default machine's Dma32 zone, Min 689 frames, gave up its last frame as readily as its first, and so did the 3388-frame Dma zone below it. The reclaimer was asked for a zone only when an order-0 walk found no frame anywhere, by which time nothing was left to hand out while it worked.

**Cause**
The watermarks were added with no consumer, and the consumer was put off twice: it needed a place in the layering (`.agents/tasks/pmm-layering/DESIGN.md` D1, the candidate walk), and then a rule. The rule first decided, decision Z3(a) of 2026-11-10, was "a request that may block stops at Low, one that may not at Min". Checked against the callers it failed both ways. Today's presets are `ALLOC_KERNEL` (with `CanBlock`) and `ALLOC_USER` (without it), so user pages would have gone deeper than kernel allocations; and with `ALLOC_USER` given `CanBlock`, as E2 had decided, no caller lacks it, so the band from Min to Low would have been unusable by anyone and every allocation would have failed at Low with nothing to wait for, since this allocator performs no direct reclaim. The user took the revised rule on 2026-11-14.

**Change**
Two passes over the same candidate zones, in `alloc_block()`:

```cpp
auto block = home.alloc_frames(request, AdmissionPass::Low);
if (!block) {
    if (!depleted) {
        depleted = block.error().depleted_zone;
    }
    block = home.alloc_frames(request, AdmissionPass::Min);
}
```

and per zone, in the node's walk, one question the zone answers about itself:

```cpp
constexpr auto admission_floor(usize min, usize low, AdmissionPass pass, bool can_block) -> usize {
    if (pass == AdmissionPass::Low) return low;   // everyone, the first time
    if (can_block) return min;                    // may block: stopped first
    return min - min / 4;                         // cannot block: nothing else to do
}
// admitted when free - 2^order >= floor
```

So Low is where reclaim is asked for, Min is the floor, and `CanBlock` decides who may go below it. When the first pass fails as a whole, the first zone it found short is named to the reclaimer, even when the second pass then succeeds, because the allocation was served below Low. A first pass that succeeds names nothing, though it may have passed over a short zone on its way to a lower one that admitted the request; reclaim then waits until no zone admits at Low, as Linux wakes kswapd only from the slow path. Every allocation form reports it once: the single-block entry after its passes, the bulk and fragment forms the first short zone across all their blocks.

`ALLOC_USER` is `CanBlock | Zero` now: user objects and pages are allocated from system calls, which may block.

Until `PmNode::setup_watermarks()` runs every watermark is zero, so no boot allocation before it can be refused.

**Reference**
Linux `get_page_from_freelist()` with `ALLOC_WMARK_LOW` on the fast path, then `__alloc_pages_slowpath()`, which wakes kswapd and retries with `ALLOC_WMARK_MIN`, and lets a caller without `__GFP_DIRECT_RECLAIM` go a quarter further (`ALLOC_NON_BLOCK`, in `__zone_watermark_ok()`). Zircon has no watermarks of this kind; its `PmmNode` delays a `PMM_ALLOC_FLAG_CAN_WAIT` request at a free-memory level instead.

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, on the default and the four-node machine; host tests 184 passed, one more than before, the new `admission-smoke`. `ktest all`: four nodes 85 ran, 82 passed, 3 skipped; default machine 85 ran, 81 passed, 4 skipped, the fourth skip being the new test, which needs a second node with memory. With `MEM=3072` both images boot and `ktest PmNode.*:PmZone.*:VmoContiguous.*:LargePage.*` passes. The failure-injection build's `ktest all`: 85 ran, 82 passed, 0 failed.

The new kernel test drains one zone of another node to exactly Low, Min and three quarters of Min through the zone itself, with the reclaimer paused, and checks at each: below Low a blocking request is served by the second pass; at Min it is refused while one that cannot block is served; at three quarters of Min both are refused. Control: with the second pass removed, its first and third checks fail.

A review of the change, two narrow reviewers one after the other, checked eight behavioural points (boot before the watermarks exist, status mapping, one report per operation, the reclaim request's locking, the gate, page-table and per-CPU allocations under pressure, the floor arithmetic, a large order against a zone near Low) and the new tests' expectations. It found three comments false, corrected here, and the `CanWait` consequence listed below.

One run of `ktest all` on the default machine failed `Steal.IdleCpuTakesQueuedThreadsByNodeAndAffinity`, a scheduler test of work stealing that allocates nothing on the path it checks; it passed in four runs of `ktest Steal.*` and in the next full run, and is recorded as a flake in `docs/repairs/261114/kernel-task-sched.md`.

**Deliberately not done**
- The low-zone reserve, which the next step adds to the floor.
- `Gaf::CanWait` still meets the machine-wide gate and not the zone's Low floor; making a waiting request stop at Low and get `ShouldWait` is decision N1. Until then a waiting request that gets past the gate and finds every zone below Min gets `OutOfMem`, where before this change it got a frame: the gate only puts a request off when free memory is below its delay level, and nothing ties that level to the zones' Min.
- Whether a rollback returns the very frame it took on an exhausted node is no longer tested there: nothing past the floor can be taken through `mm::`. The batch errors on a healthy node still exercise the rollback.

**How it was found**
None. Decision Z3 of `.agents/tasks/pmm-redesign/DESIGN.md`, revised as above.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the rule first decided ("may block stops at Low") leave the band between Min and Low unused once every preset carries `CanBlock`?
- An allocation succeeds in the second pass. Why is reclaim still asked for?
- Why does a caller that cannot block get to go deeper than one that can, when the one that can block cannot actually wait in this allocator?

## Low zones keep a reserve from requests that could go higher (2026-11-14)

**Files**
- `kernel/mm/include/ours/mm/priv/admission.hpp` — `lowmem_reserve()`, the reserve with every input an argument
- `kernel/mm/tests/admission_smoke.cpp` — the reserve on the default and the 3 GiB machine, and that it sits on top of the floor
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `m_lowmem_reserve`, one entry per highest zone type a request may allow; `setup_lowmem_reserve()`; `lowmem_reserve(highest)`; `LOWMEM_RESERVE_RATIO`; `admits()` takes the request's highest zone and adds the reserve to the floor
- `kernel/mm/pm_node_init.cpp` — `PmNode::setup_watermarks()` sets every zone's reserve after its watermarks
- `kernel/mm/pm_node.cpp` — the walk passes `request.highest_zone()`
- `kernel/tests/pm_node_test.cpp` — new `PmNode.LowZonesKeepAReserve`

**Symptom**
After the last two changes a node's highest zone is tried first and every zone is held at a floor, but nothing distinguished the low zones once the zones above them were short: an ordinary request then took a low zone down to the same Min an ordinary zone keeps. On the default machine that is all but 19 of the Dma zone's 3388 frames, the only memory a device limited to the first 16 MiB can use.

**Cause**
The order fixes which zone is spent first, not how much of a low zone may be spent by a request that did not need it. A request whose highest allowed zone is Normal could have been served from Normal or Dma32; the Dma zone is the last place it should land, and when it does land there it should not be allowed to take what a request that can only use Dma would need.

**Change**
Each zone keeps back, from a request whose highest allowed zone is `h`, one frame in 256 of what the zones above it on its node manage up to `h`:

```cpp
constexpr auto lowmem_reserve(usize const *managed, usize count, usize own, usize highest,
                              usize ratio) -> usize {
    usize above = 0;
    for (auto type = own + 1; type <= highest && type < count; ++type) {
        above += managed[type];
    }
    return above / ratio;
}
// admitted when free - 2^order >= floor + reserve[request's highest zone]
```

The table is worked out once, after the watermarks, from managed counts that are final by then, and read without a lock after. It is added in both passes, so even a request that cannot block, allowed to a quarter below Min, does not reach a low zone's reserve unless it allows nothing higher.

What the zones keep from a request that allows Normal, which is every request today:

| Machine | Dma | Dma32 | Normal |
|---|---|---|---|
| default, 512 MiB | 478 of 3388 | 0 (no Normal zone) | none |
| 3 GiB | 2996 of 3388 | 2500 | 0 |

The ratio is Linux's default for its DMA and DMA32 zones (decided with the user, 2026-11-14).

**Reference**
Linux `setup_per_zone_lowmem_reserve()` and `sysctl_lowmem_reserve_ratio[]` in `mm/page_alloc.c`, with `z->lowmem_reserve[highest_zoneidx]` added to the mark in `__zone_watermark_ok()`.

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 184 passed, on the default and the four-node machine. `ktest all`: default 86 ran, 82 passed, 4 skipped; four nodes 86 ran, 83 passed, 3 skipped; one more than before on each, the new test. With `MEM=3072` both images boot, the zones log the reserves in the table, and `ktest PmNode.*:PmZone.*:VmoContiguous.*:LargePage.*` passes. The failure-injection build's `ktest all`: 86 ran, 83 passed, 0 failed.

The new kernel test checks every zone's table against the formula from the live managed counts, then drains a low zone that keeps something back to exactly Low plus that reserve, through the zone itself and with the reclaimer paused: a request that allows Normal is refused there, one that allows nothing above the zone is admitted. Control: with the reserve left out of `admits()`, the refusal check fails.

A review by one narrow reviewer checked eight points (the reserve's indexing and bounds, that it reads only the zone's own node and runs after the managed counts are final, that only Dma and Dma32 keep anything today, both passes, the new test's expectations and frees, the log line, the comments) and found nothing.

**Deliberately not done**
- Different ratios per zone. Linux gives Normal a reserve against Movable (ratio 32); no request here allows Movable, so every Normal zone keeps 0 whatever its ratio.
- A request that allows only a low zone. No allocation flag asks for one yet (`Gaf::Dma32` is the next item); when one does, its highest zone is the low zone and the reserve against it is 0, which is the point.

**How it was found**
None. The second half of decision Z3, `.agents/tasks/pmm-redesign/DESIGN.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the Dma zone keep 478 frames on the default machine and 2996 on the 3 GiB one, when it is the same zone?
- Why is the reserve indexed by the request's highest allowed zone and not by the zone it would come from?
- Why is the reserve added in both passes, even for a request that cannot block?

## A waiting request stops at Low, and a release back above Low wakes it (2026-11-14)

**Files**
- `kernel/mm/pmm.cpp` — `alloc_block()` takes `can_wait`, and a request that asked for `Gaf::CanWait` makes only the first pass, answering `ShouldWait` when no zone admits it at Low; `free_block()` wakes one waiter when the zone says the release brought it back to admitting a frame at Low, and counts it in `pm.zone.relief`
- `kernel/mm/pm_zone.cpp`, `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_node.cpp`, `kernel/mm/include/ours/mm/pm_node.hpp` — `PmZone::free_frames()` returns whether this release crossed the zone's Low-pass line, from its own atomic increment, and `PmNode::free_frames()` passes it up
- `kernel/mm/include/ours/mm/pmm.hpp`, `kernel/mm/include/ours/mm/alloc_flags.hpp` — the two things that put a waiting request off, and the two things that wake it
- `kernel/tests/pm_node_test.cpp` — new `PmNode.WaitingRequestsStopAtLowAndWakeOnRelief`; the remote-node choice is a helper of its own
- `kernel/tests/pm_zone_test.cpp`, `kernel/tests/pm_node_test.cpp` — the tests that give frames straight back to a zone discard its answer, with `auto _`
- `docs/repairs/261112/mm.md` — new item 3, a confined waiter nothing can relieve

**Symptom**
After the watermark change, a request that could wait (a VMO page, `ALLOC_USER_VMO`) and got past the machine-wide gate went on, like any other, to the second pass and down to Min, and was answered `OutOfMem` below that. It was never told to wait for a zone. That happens when the gate's watcher has not armed it yet, when the request is confined to one node, and on a machine large enough that the zones' Low watermarks add up to more than the gate's level (49 MiB by default).

**Cause**
The gate and the zone floors answer different questions, and the change before this one connected only one of them to waiting. The gate is a policy over the whole machine: while free memory is below the level the memory watchdog set (just under its OutOfMemory level), user-driven growth is put off so that the system, and user space through the pressure it is told about, can react. A zone's Low is a local fact: this zone is short and reclaim has been asked for. A request that can wait should stop at either, and only the gate stopped it.

The first design of this step, decision N1(a), removed the gate's level and put waiting requests off at Low alone. It was set aside on 2026-11-14: on the default machine it would have let VMO pages take memory down to about 3.4 MiB before waiting, where today they wait at 49 MiB, and it would have dropped the machine-wide pressure model the watchdog was built on.

**Change**
The gate stays. A waiting request is put off at Low as well:

```cpp
// alloc_block(), after the first pass failed
if (can_wait) {
    return ustl::err(Status::ShouldWait);   // wait for a zone, keep off the Low..Min band
}
block = home.alloc_frames(request, AdmissionPass::Min);
```

So the band between Low and Min is left to the requests that cannot wait, which is what it is for. The waiter sleeps on the gate's event, as it does when the gate is shut, and a release wakes it:

```cpp
// PmZone::free_frames()
auto const before = m_free_frames.fetch_add(count);          // what this release found
auto const floor = watermark(Watermark::Low) + lowmem_reserve(ZoneType::Normal);
return !mm::admits(before, 0, floor) && mm::admits(before + count, 0, floor);

// free_block()
if (node->free_frames(frame, order)) {
    GATE.pass_on();   // one waiter retries
}
```

Only the release that crosses the line wakes anyone; a zone above it wakes nobody. The line is the one the Low pass uses for a single frame and a request that allows up to Normal, every request made today: at exactly Low the pass still refuses, because the frame would leave Low - 1, so the wake comes with the release that takes the zone to Low + 1. The zone answers from the count its own atomic increment found, not from two reads around the release: increments and decrements fall in one order, so each time the count climbs over the line exactly one release did it and only that one says so. The event resets itself when a waiter takes it, and a signal with no waiter stays set until one comes, so a release that lands between a request's `ShouldWait` and its wait is not lost: the waiter finds the event set and retries at once. A waiter that retries and is put off again waits again, as it does at a shut gate; one that gets its frame passes the wake on.

**Reference**
Linux has no separate waiting class here: `__alloc_pages_slowpath()` wakes kswapd and a caller that may block reclaims or throttles itself (`reclaim_throttle()`), woken when kswapd brings the node back over its high watermark. Zircon's `PmmNode` puts a `PMM_ALLOC_FLAG_CAN_WAIT` request off while its free count is below the delay level and wakes waiters only through the memory watchdog: the free path signals the watchdog's event when the count rises above the upper bound it was armed with (`IncrementFreeCountLocked()`), and the watchdog thread re-arms the levels with `SetFreeMemorySignal()`, which signals `may_allocate_evt`. Zircon has no per-zone line; the relief wake here signals the waiters' event directly from the free path, where Zircon signals the watchdog's.

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 184 passed, on the default and the four-node machine. `ktest all`: default 87 ran, 82 passed, 5 skipped; four nodes 87 ran, 84 passed, 3 skipped, `MemoryPressure.TheGateTheSignalAndTheWatchdog` among the passes. With `MEM=3072` both images boot and `ktest PmNode.*:MemoryPressure.*:PageReclaim.*` passes. The failure-injection build's `ktest all`: 87 ran, 83 passed, 0 failed. clang-tidy reports nothing in `pmm.cpp`, `pm_zone.cpp` or `pm_node.cpp`. The numbers are the same after the review's fixes.

The new test drains another node's zone to Low through the zone itself, with the reclaimer paused: a waiting request is answered `ShouldWait` and one that cannot wait is served. Giving back the frame the second took brings the zone to exactly Low and counts no wake, and the waiting request is still answered `ShouldWait` there; the next release, above Low, counts one; one more counts none. Controls: with waiting requests let into the second pass, the put-off and wake checks fail; with the wake removed, the wake check fails; with the line one frame early (the first version of this change), the exactly-Low and the above-Low checks fail.

A narrow pre-commit review found two faults in the first version, both fixed here: the wake fired at exactly Low, where the waiter is still refused, so it retried, slept again, and the release that did let it in woke nobody; and the comment claimed racing releases could not both miss a crossing, which two reads around the release cannot promise. It also checked that a wake signalled before the waiter sleeps is kept by the self-resetting event, that a waiter woken at a shut gate waits again without spinning, that the zone is known for every freed frame, and that nothing on the free path holds a zone lock when it signals. Not proven: no free under a scheduler or wait-queue lock was found by search, but signalling from such a free would take those locks again.

**Deliberately not done**
- A waiter confined to one node that nothing can relieve waits for ever; recorded as `docs/repairs/261112/mm.md` item 3.
- The gate's level is still the watchdog's, with no relation to the zones' watermarks.

**How it was found**
None. Decision N1 of `.agents/tasks/pmm-redesign/DESIGN.md`, revised as above.

**The user's prediction**
None.

**Three questions you should be able to answer**
- The gate and a zone's Low both put a waiting request off. What question does each answer, and why is neither enough alone?
- A release lands after a request was answered `ShouldWait` and before it went to sleep. Why does the waiter not sleep through it?
- Why does a waiting request skip the second pass, when a kernel request makes it?

## The record of a short zone moves into its node (2026-11-14)

**Files**
- `kernel/mm/include/ours/mm/pm_node.hpp`, `kernel/mm/pm_node.cpp` — `PmNode::mark_short()` and `PmNode::take_reclaim_target()`, over a new `m_short_zones`, one atomic word with a bit per zone type
- `kernel/mm/include/ours/mm/memory_reclaimer.hpp`, `kernel/mm/memory_reclaimer.cpp` — `m_zone_requests` and `RequestState` go; `request(PmZone const &)` marks the zone's node and signals on the first mark; the thread takes each node's target; while a Pause lives the thread takes no node's record, and the last Pause to end wakes it
- `kernel/tests/pm_node_test.cpp` — new `PmNode.AShortZoneIsMarkedOnceAndTakenOnce`
- `docs/repairs/261114/kernel-task-sched.md` — a second sighting of the steal flake, on another check

**Symptom**
None in behaviour. The reclaimer kept its own `m_zone_requests[MAX_NODES][NR_ZONES_PER_NODE]` of Idle and Pending states, indexed by raw node and zone ids: a second copy of the node topology, outside the nodes, holding state about the nodes' zones.

**Cause**
When allocations first reported a short zone, the reclaimer was the only thing that read the report, so the report was kept where it was read. Since the watermark change, a failed Low pass is what asks for reclaim, and the next step (`docs/repairs/261112/mm.md` item 3) needs a node to know that reclaiming it gave nothing back. Both facts are about the node, and decision N2 of `.agents/tasks/pmm-redesign/DESIGN.md` put them there.

**Change**
The node records and answers; the reclaimer wakes itself (option (b) of N2, the user's choice: the node does not call the reclaimer above it).

```cpp
// PmNode
auto mark_short(PmZone const &zone) -> bool {        // true: first mark since the last take
    return m_short_zones.fetch_or(bit(zone), acq_rel) == 0;
}
auto take_reclaim_target() -> usize {                // clears the record
    auto const marked = m_short_zones.exchange(0, acq_rel);
    // sum High - free over the marked zones, read now
}

// MemoryReclaimer::request(zone), from observe_pressure()
if (!started || paused()) { return; }                // before the mark, not after
if (find_node(zone.which_node())->mark_short(zone)) { m_signal.signal(); }
```

The invariant is that a record that is not empty always has a wake on its way: every mark that finds the word at zero is followed by a signal, and the self-resetting event keeps a signal that finds the thread busy. That is why the started and paused checks come before the mark. A mark nothing will take stays set, every later mark on the node answers that a wake is already out, and the thread would never be told again. One word per node rather than one atomic per zone: one exchange takes the node's whole record, and one fetch_or tells a marker whether it is the first. Two zone types marked before a take now cost one signal where they cost two.

The Pause change came out of the review. A Pause dropped reports but did not stop the thread from taking a node's record when a wake already in flight arrived, so a test reading a record under a Pause could have it taken away. Now the thread skips the node records while a Pause lives, and the last Pause to end signals it, so a mark that slipped in at the Pause's start is taken afterwards rather than left set with its wake spent.

**Reference**
Linux keeps the request in the node: `wakeup_kswapd()` writes `pgdat->kswapd_highest_zoneidx` and `pgdat->kswapd_order` and wakes `pgdat->kswapd_wait`, and `kswapd()` reads both and resets them to `MAX_NR_ZONES` and 0 before it balances the node (`mm/vmscan.c`). The record here is a set of zone types rather than a highest index and an order, there is one reclaimer thread rather than one per node, and the take is one atomic exchange where Linux's reads and resets may lose a racing request. Zircon has no zones and no such record: eviction is asked for by the memory watchdog over the whole machine, which `request(frames)` already follows.

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 184 passed, on the default and the four-node machine. `ktest all`: default 88 ran, 82 passed, 6 skipped (the new test needs a second node); four nodes 88 ran, 85 passed, 3 skipped, after one run where `Steal.IdleCpuTakesQueuedThreadsByNodeAndAffinity` failed (the filed flake, on another check). With `MEM=3072` both images boot and `ktest PmNode.*:MemoryPressure.*:PageReclaim.*` passes, 12 ran and 8 passed. The failure-injection build: 88 ran, 83 passed, 0 failed; the run is reported as failed only because of the three deliberate `Failed to map` lines, which the same suites print without this change (`docs/repairs/261105/kernel-test-framework.md`). clang-tidy reports nothing in `pm_node.cpp` or `memory_reclaimer.cpp`.

The new test marks another node's zone and takes it under a Pause: the first mark says it is the first and a second does not; a marked zone above High asks for nothing; drained to Low, the next mark is the first again and the take answers High - Low; a second take answers 0. Controls: with `mark_short()` always answering true, the first check fails; with the take reading the word without clearing it, the after-a-take and the clears-the-record checks fail.

A narrow pre-commit review checked the lost-wake invariant (including a Pause beginning or ending between the check and the mark), equivalence with the old code, that the asserted zone always belongs to the node it is marked on, the ordering comment, and layering. It found the test could have its record taken by a wake in flight, fixed by the Pause change above, and a header comment that called a report before start "dropped as an ask is", when an ask before start is kept.

**Deliberately not done**
- The node does not yet record that reclaiming it gave nothing back; that is `docs/repairs/261112/mm.md` item 3, the next step, and it builds on this record.
- No test drives a report end to end through the thread to a reclaim on that node; none did before either.

**How it was found**
None. Decision N2 of `.agents/tasks/pmm-redesign/DESIGN.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must the started and paused checks in `request(zone)` come before `mark_short()` and not after?
- One word with a bit per zone type, or one atomic per zone: what does the single word buy a marker, and what does it buy the thread?
- A test reads a node's record under a Pause. Which wake could still take it away before this change, and what ends the Pause safely now?

## The watchdog's levels are raised above the zones' floors (2026-11-14)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `PmZone::normal_low_floor()`, the Low pass's floor for a request that allows up to Normal; `free_frames()` uses it for its crossing line
- `kernel/mm/include/ours/mm/pmm.hpp`, `kernel/mm/pmm.cpp` — new `zone_floor_frame_count()`
- `kernel/mm/include/ours/mm/memory_watchdog.hpp`, `kernel/mm/memory_watchdog.cpp` — new `MemoryPressureLevels::covering()`; `from_boot_options()` returns the boot levels covered by the zones' floors; the watchdog logs when it raised them
- `kernel/tests/memory_pressure_test.cpp` — new `MemoryPressure.TheBootLevelsCoverTheZoneFloors`

**Symptom**
On a machine of about 6 GiB or more, a waiting request (`ALLOC_USER_VMO`, a VMO page) could sleep for ever once memory ran out. Booted at 8 GiB, the zones' floors for an ordinary request are Dma 3386 frames (its reserve of 8036 is more than the zone), Dma32 302 + 7540 and Normal 4619: when every zone refuses, about 61 MiB is still free. The gate shuts below its delay level of 49 MiB and the watchdog declares OutOfMemory below 50 MiB, so neither ever acts; each waiter was put off at a zone's Low and only a release back above Low could wake it.

**Cause**
The watermark change and decision N1 gave waiting requests a second, local threshold (a zone's Low), while the thresholds that end a wait (the gate's delay level and the OutOfMemory refusal) stayed machine-wide and fixed by boot option. Nothing related the two. The zones' floors grow with memory (Low by about 0.2 % of a zone, the low zones' reserve by 1/256 of what is above them), the boot levels do not, and past about 6 GiB the floors summed exceed the delay level. Between the two lies a state where every zone refuses and the machine-wide count still looks healthy. The N1 record listed the missing relation as not done; this is what it cost.

**Change**
The decision is the user's (D1(a), 2026-11-14): keep N1 and keep one machine-wide line, but put the line above the floors.

```cpp
// pmm.cpp: the most free memory there can be while every zone refuses
total += std::min(zone.normal_low_floor(), zone.present_frames());

// memory_watchdog.cpp
auto const needed = held + m_debounce + 1;        // delay_level() = oom - debounce > held
if (m_out_of_memory >= needed) { return *this; }
auto const raise = needed - m_out_of_memory;      // every level up by the same amount
```

The argument, for one frame and a request that may use every zone up to Normal on any node: a zone refuses such a request in the Low pass exactly when its free count is at or below its floor, and its free count is never above its present frames. So when every zone refuses, the machine's free count is at most the sum, which is now below the delay level: the request that tries shuts the gate, the watchdog is woken (Critical's lower bound is the delay level), and at OutOfMemory the waiters are refused. The converse holds too: a request the gate lets through finds a zone that admits it, unless the counts moved between the two reads. So for ordinary requests the gate decides when to wait, and a zone's Low catches only that race; requests of a higher order or confined to fewer nodes or zones are not covered (the next change is about confined ones).

Two details. The cap is present frames, not managed: managed grows after boot as reserved frames are given back (the `.init` sections, the boot image), and the first version, capped at managed, came out one frame short at 8 GiB once that happened. All three levels move by the same amount, so each pressure band keeps the width the command line gave it; levels that do not nest are left alone, so a bad command line is still refused by `start()` and not repaired. The 512 MiB and 3 GiB machines keep the configured levels (floors 1362 frames and about 30 MiB); at 8 GiB the levels go up 13 MiB.

**Reference**
Neither system has this pair. Zircon has no zones: its free-memory levels are the only thresholds, set by `kernel.oom.*`, and a `PMM_ALLOC_FLAG_CAN_WAIT` request waits on them alone. Linux has the zone watermarks and no machine-wide level for allocations: a caller that can reclaim does so itself, and the OOM killer is reached when reclaim makes no progress (`should_reclaim_retry()`, `__alloc_pages_may_oom()`). The relation kept here is this kernel's own, because it has both.

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 184 passed, on the default and the four-node machine. `ktest all`: default 89 ran, 83 passed, 6 skipped; four nodes 89 ran, 86 passed, 3 skipped. `MEM=3072` and `MEM=8192`: `ktest PmNode.*:MemoryPressure.*:PageReclaim.*` 13 ran, 9 passed, 0 failed; at 8 GiB the boot log says `kernel.oom levels raised 13 MiB: the zones' floors hold 61 MiB` and the test reports floors of 16213 frames under an OutOfMemory level of 16470 with a debounce of 256. Failure-injection build: 89 ran, 84 passed, 0 failed, with the three known `Failed to map` lines. clang-tidy reports nothing new in `memory_watchdog.cpp`, `pmm.cpp` or `pm_zone.cpp`.

The new test checks `covering()` on small levels (below, at and far above the delay level, bands kept) and that the boot levels and the levels in force keep the delay level above the floors. The second half only bites where the floors exceed the configured delay level, about 6 GiB and up, which is why the 8 GiB machine is part of this verification: with `from_boot_options()` returning the levels uncovered as a control, both of its checks fail there.

A narrow pre-commit review confirmed the argument (every zone counted on both sides, the floors fixed after boot, the tests that restore the boot levels compare nothing against the raw options) and that an ordinary request let through the gate is put off at Low only by a race. It found that `covering()` would have repaired an OutOfMemory level at or below the debounce, fixed by leaving non-nesting levels alone; that the test called `delay_level()`, which asserts nesting, before asking `is_valid()`; and a comment in `pmm.hpp` that claimed the gate guarantees a zone to every waiting request. Its top finding, that `from_boot_options()` was not wired, came from reading the tree while the control above was in place.

**Deliberately not done**
- The default verification boots 512 MiB and 3 GiB, where the raise never happens; nothing runs the 8 GiB check routinely.
- A waiting request of a higher order, or confined to fewer nodes or zones, can still be put off at Low with the gate open; confined ones are the next change.

**How it was found**
The critic run while preparing `docs/repairs/261112/mm.md` item 3 worked out the floors for large machines; an 8 GiB boot confirmed them.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is "free at most the sum of the floors when every zone refuses" true, and why does the cap have to be present frames rather than managed?
- With the delay level above the floors, when can an ordinary waiting request still be put off at a zone's Low?
- Why move all three levels by the same amount instead of only the OutOfMemory level?

## The reserve test picks a zone it can drain (2026-11-14)

**Files**
- `kernel/tests/pm_node_test.cpp` — `PmNode.LowZonesKeepAReserve` chooses the first zone with a reserve that has more free frames than its Low floor plus that reserve

**Symptom**
At 8 GiB `PmNode.LowZonesKeepAReserve` failed its admission check: "at Low plus its reserve a low zone refuses a request that allows Normal".

**Cause**
The test took the first zone with any reserve against Normal and drained it to Low plus that reserve. At 8 GiB that zone is Dma, whose reserve (8036 frames) is larger than the whole zone (3386), so the drain could never reach the line and `drain_to()` reported failure. It was written and checked at 512 MiB and 3 GiB, where Dma's reserve still fits.

**Change**
The zone must have more free frames than `normal_low_floor()`; at 8 GiB that is Dma32. With no such zone the test skips.

**Reference**
None, this project's own test.

**Verification**
`MEM=8192`, `MEM=3072` and the default machine: the test passes; the four-node `ktest all` 89 ran, 86 passed.

**Deliberately not done**
None.

**How it was found**
Running the 8 GiB machine for the change above.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a low zone's reserve be larger than the zone itself, and what does that zone then give an ordinary request?
- Why does the test drain a low zone rather than the highest one?
- What would the test have proved had it drained Dma32 at 512 MiB, where Dma32 keeps nothing back?

## A confined waiting request goes on to Min instead of waiting at Low (2026-11-14)

**Files**
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` — new `AllocRequest::is_confined()`: `Gaf::ThisNode`, or a zone range narrower than Dma through Normal
- `kernel/mm/pmm.cpp` — `alloc_block()` puts a waiting request off at Low only when it is not confined
- `kernel/mm/include/ours/mm/pmm.hpp`, `kernel/mm/include/ours/mm/alloc_flags.hpp` — what `ShouldWait` means for a confined request
- `kernel/tests/pm_node_test.cpp` — new `PmNode.AConfinedWaitingRequestGoesOnToMin`; `PmNode.WaitingRequestsStopAtLowAndWakeOnRelief` asks through a one-node stand-in machine and now also checks that the woken request is served
- `kernel/mm/tests/alloc_request_smoke.cpp` — which requests are confined
- `docs/repairs/261112/mm.md` — item 3 removed; `docs/repairs/261114/mm.md` — the two holes it leaves, both latent

**Symptom**
Latent, no caller asked for it: a waiting request confined to one node (`ALLOC_USER_VMO | Gaf::ThisNode`) on a node whose zones were all below Low was answered `ShouldWait`, and only a release on that node back above Low could wake it; with nothing reclaimable there it waited for ever (`docs/repairs/261112/mm.md` item 3). A confined request whose node set left it no node at all (`ThisNode` with its home outside the set) found no zone, named none short, and was answered `ShouldWait` too, waiting on nothing.

**Cause**
The wait at a zone's Low (N1) is only safe when something is bound to end it. For an ordinary request the previous change made sure of that: every zone refusing it means the machine is below the gate's delay level, where the gate and the watchdog act. A confined request counts against memory it may use, while the gate and the watchdog count the machine, so its zones can all be short while the machine looks healthy.

**Change**
The user's D2(b), 2026-11-14: a confined waiting request is not put off at Low. It makes the Min pass like a request that cannot wait, still asks for reclaim for the zone it found short, and answers `OutOfMem` below Min.

```cpp
// alloc_block(), after the Low pass failed
if (can_wait && !request.is_confined()) {
    return ustl::err(Status::ShouldWait);
}
block = home.alloc_frames(request, AdmissionPass::Min);

// AllocRequest
return !!(m_alloc_flags & Gaf::ThisNode) || m_lowest_zone != ZoneType::Dma ||
       m_highest_zone < ZoneType::Normal;
```

So a confined request trades the wait for the Low-to-Min band: it spends memory the requests that cannot wait depend on, and fails where an ordinary one would have waited. A caller asking for one node already accepts failure (`Gaf::ThisNode` says so). The zone-range half answers nothing today, since no `Gaf` bit narrows the range; it is there so that a request limited to memory below 4 GiB, when `Gaf::Dma32` is built, is confined from the start. The empty-node-set case now answers `OutOfMem` from both passes, with no zone named and so no reclaim asked.

The first draft of this fix, the one item 3 described, was a per-node "reclaim gave nothing back" mark that a retry would turn into `OutOfMem`. Worked through before this step, it needed five parts to be correct: the home carried through the wait, a generation instead of a bit (the bit can be set after the crossing that should clear it), "the queues ran out" instead of "freed nothing", a reclaim that says when it did not run, and a periodic retry because the one-at-a-time wake stops at the first waiter still put off. And for zone-limited waiters the node is the wrong unit. That machinery is not needed while confined requests do not wait.

The wake test changed with it. It asked with `ThisNode`, which is now confined, so it asks through `mm::priv::alloc_frames()` with the target node as the whole node set: the set stands in for a one-node machine and the request is ordinary. It also retries after the release that takes the zone above Low, and sees the request served, where it used to check only that the relief counter moved.

**Reference**
Neither system has the case. Zircon has no node-confined allocation, and its `PMM_ALLOC_FLAG_CAN_WAIT` waits on machine-wide levels alone. Linux's `__GFP_THISNODE` allocations make no wait of this kind at all: they reclaim or fail, and are excluded from the OOM killer (`__alloc_pages_may_oom()` returns early for them), which is the same trade, failure in place of an unbounded wait.

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 184 passed (`alloc-request-smoke` with its five new checks), on the default and the four-node machine. `ktest all`: default 90 ran, 83 passed, 7 skipped (both node tests need a second node); four nodes 90 ran, 87 passed, 3 skipped. `MEM=3072` and `MEM=8192`: `ktest PmNode.*:MemoryPressure.*:PageReclaim.*` 14 ran, 9 passed, 0 failed. Failure-injection build: 90 ran, 84 passed, 0 failed, with the three known `Failed to map` lines. clang-tidy reports nothing in `pmm.cpp`.

The new test drains another node's zone with the reclaimer paused: at Low a confined waiting request is served by the Min pass from that node; at Min it answers `OutOfMem`; with a node set that leaves it no node it answers `OutOfMem` at once. Control: with the confinement check removed, all three checks fail (the first two get `ShouldWait`, the third `ShouldWait` on nothing).

A narrow pre-commit review confirmed that no caller combines `CanWait` with `ThisNode`, an explicit home or a node set, that nothing relies on a confined request answering `ShouldWait`, and that the empty-set case reports no zone. It found the stale header comment on `alloc_block()`, the wording of `is_confined()` (it promised "less memory" while checking only the flag and the range), and the wake test's missing retry, all fixed; and two latent holes, recorded in `docs/repairs/261114/mm.md`: a confined request put off by the gate retries on the waking CPU's node, and a node set narrowed without `ThisNode` is not treated as confined.

**Deliberately not done**
- The wait path still carries only the flags; `docs/repairs/261114/mm.md` item 1.
- A future placement policy that narrows the node set has to narrow the request, or the rule has to look at the node set; item 2 there.

**How it was found**
None. `docs/repairs/261112/mm.md` item 3, decided with the user as D2(b).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the machine-wide gate end an ordinary request's wait but not a confined one's?
- What does a confined waiting request give up by going on to Min, and why is that acceptable for `Gaf::ThisNode`?
- The wake test now asks through a one-node node set without `ThisNode`. Why is that request ordinary, and what would make the test change meaning later?

## The Dma32 zone ends at 4 GiB (2026-11-14)

**Files**
- `kernel/arch/x86/kernel/include/ours/arch/mem_cfg.hpp` — `ARCH_MAX_DMA32_PFN` is 4 GiB, with a comment saying what the bounds are
- `kernel/mm/include/ours/mm/pm_node.hpp` — the fallback-order comment's example address limit
- `kernel/mm/tests/admission_smoke.cpp` — the comment on where its 640150 frames of Normal came from
- `.agents/tasks/pmm-redesign/DESIGN.md` — the bound fixed, `Gaf::Dma32` and `Gaf::Movable` deferred, and what the survey found about both

**Symptom**
The Dma32 zone, the memory a device with 32-bit addresses can reach, ended at 512 MiB. On the 3 GiB machine it held 122515 frames of the roughly 520000 below 4 GiB, and everything from 512 MiB to 4 GiB was Normal: an ordinary request took it first, and the low-zone reserve protected an eighth of what a 32-bit device could use.

**Cause**
The value came with the first commit (1387a1a3, as `MB(512)`) and nothing records why. Nothing read it but the platform's zone-bound table (`kernel/platform/pc/memory.cpp`); phys keeps its own 4 GiB bounds, the early allocator and the physmap use no zone bound, and that table's own comment already said "DMA32's 4 GiB".

**Change**
`ARCH_MAX_DMA32_PFN` is `4_gb >> PAGE_SHIFT`. The user chose the bound alone (2026-11-14): `Gaf::Dma32` and `Gaf::Movable` stay unbuilt until something asks for them.

What moved, read from the boot logs:

| Machine | Dma32 frames | Normal frames | Dma32 keeps from Normal requests | Zones' floors |
|---|---|---|---|---|
| 512 MiB (default) | 122507, unchanged | none | 0 | 1362, unchanged |
| 1 GiB, four nodes | every node's zone is Dma32; nodes 2 and 3 were Normal | none | 0 | unchanged |
| 3 GiB | 518897, was 122515 | 248217, was 640150 | 969, was 2500 | 6487, was 7992 |
| 8 GiB | 518897, was 126972 | 1538457, was 1930384 | 6009, was 7540 | 14682, was 16213 |

At 8 GiB the watchdog's levels are now raised 9 MiB instead of 13. Below about 2.8 GiB QEMU puts all memory under 4 GiB, so the two machines `verify` boots have no zone above Dma32 now; the four-node machine used to have Normal on two nodes.

**Reference**
Linux's `MAX_DMA32_PFN` is 4 GiB (`arch/x86/include/asm/dma.h`). Zircon has no low-memory zone: it removed `PMM_ALLOC_FLAG_LO_MEM` in 2026 as unused, and leaves what a device can reach to its BTI (`Bti::aspace_size()`).

**Verification**
`verify` in the `feat/pmm-redesign` worktree: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 184 passed, on the default and the four-node machine. `ktest all`: default 90 ran, 83 passed, 7 skipped; four nodes 90 ran, 87 passed, 3 skipped; `MEM=3072` and `MEM=8192` 90 ran, 83 passed, 7 skipped each, `MemoryPressure.TheBootLevelsCoverTheZoneFloors` among the passes. Failure-injection build: 90 ran, 84 passed, 0 failed, with the three known `Failed to map` lines. The numbers in the table match a model of the watermark, reserve and floor arithmetic made beforehand to within half a percent.

**Deliberately not done**
- `Gaf::Dma32`: nothing in the kernel needs memory below 4 GiB, and its natural user, an option on `vmo_create_contiguous` for 32-bit devices, is ABI to decide with its first driver.
- `Gaf::Movable`: grouping free memory by mobility is a performance item, and nothing gives memory back by physical range yet. When it is built, it must not go into `ALLOC_USER`, which also makes every dispatcher through the slab allocator.
- `ARHC_MAX_PFN_LIMIT` beside it is misspelled; left for its own change.

**How it was found**
The survey made while introducing `Gaf::Dma32` and `Gaf::Movable`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a low zone's bound decide how much memory the lowmem reserve protects?
- Why did the Dma32 zone grow fourfold at 3 GiB but only by a third of a node's memory at 8 GiB?
- Why does no machine `verify` boots routinely have a Normal zone any more?
