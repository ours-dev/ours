# mm outstanding repairs

Raised by the review of the change that made a missing SLIT non-fatal, each confirmed by a skeptic and refused as a defect of that change because it predates it. The item that used to stand here -- an SRAT without a SLIT treated as no topology at all -- was fixed on 2026-10-29; see `docs/changes/261029/mm.md`.

## 1. A memory-affinity walk that fails part-way still abandons ACPI discovery after it has registered nodes

**Severity**
Medium. Reachable from firmware data alone, with the same consequence as the no-SLIT bug just fixed: no CPU bound to a node, every node's hard affinity empty, and a boot log stating there was no topology while the kernel brings up the nodes it registered.

**Symptom**
Latent on the machines booted today: QEMU never describes a memory region bootmem refuses. On a machine that does, the log carries `numa: no firmware topology` above `pmm: N node(s) own memory` with N greater than one, and `pmnode.test` reports `0 of M possible CPU(s) were placed by firmware`.

**Cause**
`register_meminfo()` commits as it goes and reports at the end. For each region it calls `EarlyMem::set_node()`, which rewrites the live bootmem region list, and then `PmNode::set_state(..., Possible)`. When one region is refused it latches the error and skips the rest, and `acpi_init_numa()` returns that error before `register_nodedis()` and `bind_node_to_cpu()` run. Nothing undoes the regions already re-homed or the nodes already marked, `find_possible_nodes()` reads those marks, and `x86_init_numa()` logs that there was no firmware topology.

Three firmware shapes reach it: an enabled memory-affinity entry of length 0 (`bootmem::Range::from()` refuses a zero size with `InvArgs`), a range `assign_node()` cannot split into because the region list is full (`NoMemory`), and an entry list that turns malformed part-way (`for_each_memory_region()` visits the good entries first and reports `InternalError` after).

The change record for the SLIT fix says so; its first draft claimed this case hands the PMM nothing, which is false.

**Change**
Decide what a half-applied memory map should become -- a decision for the user, because the two answers lead to different structures:

- Carry on: treat a refused region like a truncated one -- count it, warn, keep going -- and return `Ok`. Consistent with the two changes before it, and small.
- Stage, then commit: collect the regions into a list first, validate the whole list, and only then re-home them, so that an error really means nothing was applied. The more principled shape, and the list is plain data that a host test can drive with a synthesized SRAT.

**Reference**
Linux's `acpi_numa_memory_affinity_init()` does not touch memblock as it parses; it appends to a `numa_meminfo` array through `numa_add_memblk()`, and `numa_init()` validates and registers that array only after the whole parse succeeded, discarding it and falling back to `dummy_numa_init()` otherwise. That is the stage-then-commit shape.

**Acceptance**
Not reachable from QEMU. With staging, a host case over a synthesized SRAT whose third region is zero-length: either nothing is re-homed and the status is an error, or (carrying on) two regions are re-homed, one is counted as refused, and the status is `Ok`. Either way, `NUMA=1` and `NUMA=33` are unchanged.

## 2. A SLIT that describes fewer localities than the SRAT names leaves the rest at distance 0, so they lead every fallback chain

**Severity**
Low. Needs firmware whose two tables disagree, and the consequence is allocation order, not correctness.

**Symptom**
Latent: QEMU always emits a SLIT covering every node it declares. On a machine whose SRAT names 32 domains and whose SLIT declares 8 localities, node 0's fallback chain comes out 0, 8, 9, ..., 31, 1, ..., 7 -- the 24 nodes the firmware said nothing about ahead of the seven it said are nearest.

**Cause**
`register_nodedis()` writes only the leading square the SLIT describes and leaves every other entry of `numa::Topology`'s matrix as it was, which is zero. Zero is nearer than any real SLIT distance (the local distance itself is 10), and `build_fallback_orders()` takes the strictly nearest unvisited node at each step, so every undescribed node is taken before every described one.

**Change**
Fill the matrix with "local" on the diagonal and "remote" elsewhere before laying the SLIT over it, instead of zero. An undescribed pair then reads as remote, which is the honest default, and the no-SLIT case comes out in node-id order exactly as it does now, so `reset_all_distances()` would become that fill.

**Reference**
Linux's `numa_alloc_distance()` in `arch/x86/mm/numa.c` pre-fills its distance table with `LOCAL_DISTANCE` on the diagonal and `REMOTE_DISTANCE` elsewhere, and `numa_set_distance()` then overwrites only the pairs the SLIT gives.

**Acceptance**
A host case in `kernel/lib/numa` over a matrix whose leading 2x2 square is filled and the rest defaulted: node 0's fallback order is 0, 1, then the rest in id order. `NUMA=1`'s fallback dump is unchanged.
