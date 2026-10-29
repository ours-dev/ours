# tooling changes

## NUMA names a node count, so a boot can declare more proximity domains than the kernel holds (2026-10-29)

**Files**
- `scripts/qemu_boot.py` — `NUMA` is read as a node count rather than a flag; `numa_args()` takes the count, generates the nodes above 1, and keeps the designed four-node machine byte-identical.

**Symptom**
None: new test capability. What it makes testable was previously untestable -- `kernel/arch/x86/mm/srat.cpp` has two paths for firmware that names more proximity domains than `MAX_NODES`, and no machine this repo can boot reaches either. The clamp committed earlier today (`da8ea59f`) had to be accepted on a host case and an argument, with its kernel-side path never executed.

**Cause**
None, this is not a fix. The reason it did not exist: `NUMA=1` was written as `os.environ.get("NUMA", "0") != "0"`, a flag selecting one hand-written four-node topology, chosen for its deliberately non-uniform distance matrix. That machine is the right one for the fallback ordering and the wrong one for everything about the node ceiling, because four is below every limit in the kernel.

**Change**
`NUMA` is now a count. `0` is a uniform machine, `1` is still the designed four-node topology with its 10/20/30 matrix, and `n >= 2` is `n` generated nodes:

```python
    numa = int(os.environ.get("NUMA") or 0)
    designed = numa == 1
    nodes = len(NUMA_NODES) if designed else numa
```

A generated machine gives the first nodes one CPU each and leaves the rest memory-only, which keeps every CPU's domain inside `MAX_NODES` -- so what `NUMA=33` truncates is memory, not CPUs, and the two paths can be told apart in the log. It declares no distances, so its firmware carries an SRAT and no SLIT: the common shape on real hardware, and one `NUMA=1` cannot produce.

Its memory default is `max(16, 1024 // nodes) * nodes`, which is just under 1 GiB however many nodes there are. That ceiling is not a choice about NUMA: the boot shim links only the first GiB of its identity map, so physload dies on any machine with more memory than that. The first `NUMA=33` attempt used 64M a node, got 2112M, and never reached the kernel. See `docs/repairs/261029/phys.md`.

Rejected: a second environment variable for the count (`NUMA=1 NODES=33`). Two variables that must agree is a third state to get wrong, and the count already implies the switch.

**Reference**
None, this project's own test harness. The topologies it describes are QEMU's `-numa node,nodeid=,cpus=,memdev=` and `-numa dist,src=,dst=,val=`; QEMU requires node ids to be contiguous from 0 (`numa: Node ID missing: 32` when they are not), which is why a machine with a domain above `MAX_NODES` has to have every domain below it too.

**Verification**
- The designed machine is unchanged, argument for argument: `numa_args(1024, 4, 8, True)` compared element-wise against the old function's output, transcribed from HEAD -- identical.
- `NUMA=1` boots both images on `out/numa.x86`: dbg 44 passed, rel 41 passed with 6/6 required. `NUMA=33` likewise: dbg 44 passed, rel 41 passed 6/6.
- `NUMA=33 scripts/kshell_run.py pmnode.test` -> `ok -- 392 checks` (56 on the four-node machine; the count scales with nodes).
- `NUMA=33` with the default 1023M is the machine on which the SRAT truncation report was observed; see the entry in `docs/changes/261029/mm.md`.
- QEMU accepts 33 nodes: probed on its own, before any boot, with a 4-second run that only parses arguments.

**Deliberately not done**
- The 1 GiB shim limit is recorded in `docs/repairs/261029/phys.md`, not fixed. It is a boot-path bug in `start64.S` found by this work, not part of it, and it wants its own change.
- A generated machine gets no distance matrix. QEMU would fill one in uniformly, but the point of this machine is the node count; the ordering is what the designed one is for.
- No `NUMA=33` run was added to `boot_check.py`'s standard pass. Whether it belongs there is a question for the user, together with `NUMA=1`.

**How it was found**
The repair item being fixed asked for "a host or kshell case over an SRAT whose domain ids exceed MAX_NODES" and there was no way to produce one. Non-contiguous node ids were probed first, because two nodes numbered 0 and 40 would have needed no new machine; QEMU refuses them.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a machine with a proximity domain above `MAX_NODES` not be built with two nodes?
- Why does a generated node get at most one CPU, and what would a CPU on node 32 have made harder to read in the log?
- Why is a generated machine's memory capped just under 1 GiB, and what breaks at 1056M?
