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
