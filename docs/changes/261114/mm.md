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
