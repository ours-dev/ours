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
