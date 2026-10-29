# mm outstanding repairs

Found while verifying step 4 of `.agents/tasks/numa-out-of-mm/PLAN.md`, which was the first time in that task a NUMA-enabled kernel was built and booted at all.

## 1. A NUMA kernel on a four-node machine still brings up one node

**Severity**
Medium. Nothing is wrong on any machine the project boots today, because the default build has `MAX_NODES == 1` and cannot represent a second node. It is the condition under which every multi-node path in the allocator is unreachable, so it blocks testing them rather than breaking them: the fallback order, a node's hard affinity, `Gaf::ThisNode`, and the per-node watermarks all have exactly one node to work with, and so pass by having nothing to distinguish.

**Symptom**
Build with `mm_numa = true` and boot under `NUMA=1`, which hands QEMU the four-node topology in `scripts/qemu_boot.py` (`NUMA_NODES`, `NUMA_DISTANCES`). Both images boot and every check passes, and `pmnode.test` reports:

```text
pmnode.test:   cpu0 is on node 0; 1 node(s) online
pmnode.test:   0 of 4 possible CPU(s) were placed by firmware
```

So the firmware placement table is empty and one node is online, on a machine presenting four of each. The distance matrix is therefore all zeroes, and the fallback order degenerates to node-id order over a single node -- an empty order.

**Cause**
Not yet diagnosed; this item is to diagnose it. What is known:

- `kernel/arch/x86/mm/srat.cpp` is the only thing that fills either table, through `register_nodedis()` (the SLIT) and `bind_node_to_cpu()` (the SRAT), and it is compiled only under `mm_numa || mm_numa_emulation`.
- It had not compiled since the library move at `ad246ae5`, because `mm_numa = true` could not be configured at all -- fixed in the same round as this item was filed, see `docs/changes/261029/mm.md`. So this code has had no compiler looking at it for the whole of `.agents/tasks/numa-out-of-mm`, and no boot exercising it for longer than that.
- Which of the three stages fails is open: ACPI not finding the SRAT and SLIT, the parse finding them and yielding nothing, or `find_possible_nodes()` not reading what the parse recorded.

The first step is a log line per stage, since the path is currently silent on success and on failure alike: `register_nodedis()` logs `"No NUMA"` only when the SLIT is absent.

**Change**
Diagnose first, then fix wherever it lands, and only then add the check that would have caught it: a kshell test or a required boot line asserting that a kernel booted on a multi-node machine brings up more than one node. That assertion is the thing actually missing -- a one-node outcome is indistinguishable from success today, which is why this went unnoticed.

Do not reach for the emulation switch (`mm_numa_emulation`) as the fix. Faking a topology would make the multi-node paths testable while leaving the firmware path just as broken, and the firmware path is the one real hardware uses.

**Reference**
Linux: `arch/x86/mm/numa.c`'s `numa_init()` drives `x86_numa_init()` through `acpi_numa_init()` (`arch/x86/mm/srat.c`), which registers each SRAT memory affinity with `numa_add_memblk()` and each SLIT entry with `numa_set_distance()`, then `numa_register_memblks()` builds the nodes. `numa_init()` falls back to `dummy_numa_init()` -- one node over all memory -- when any stage fails, and says so: `pr_info("Faking a node at [mem ...]")`. That log line is the shape of what is missing here.

**Acceptance**
```bash
python3 tools/set.py bringup.x86 --out out/numa.x86 --args 'mm_numa=true'
NUMA=1 python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/numa.x86
NUMA=1 python3 scripts/kshell_run.py --out out/numa.x86 pmnode.test
```

`pmnode.test` must report four nodes online and four CPUs placed by firmware, and `build_fallback_orders() lists every other node with memory once, nearest first` must then be checking three hops rather than none.
