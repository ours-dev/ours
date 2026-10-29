# mm outstanding repairs

Found on 2026-10-29 on the first `NUMA=33` boot, built to accept the fix for the item that used to stand here (an SRAT whose memory affinities were applied in part and reported as whole; see `docs/changes/261029/mm.md`). A machine with more proximity domains than `MAX_NODES` also declares no distances, and that is what exposed this.

## 1. An SRAT without a SLIT is treated as no topology at all, so no CPU is placed and the log denies what the kernel just did

**Severity**
High. Reachable on any machine whose firmware names proximity domains but no distances between them, which is the common shape on real hardware -- a SLIT is optional. Every node's hard affinity comes out empty, so node-local allocation has nothing to prefer, and the boot log states the opposite of what happened.

**Symptom**
`NUMA=33 python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/numa.x86`, on both images:

```text
[info] numa: no ACPI SLIT; every node reads as equidistant
[info] numa: no firmware topology (acpi NotFound, pci NotSupported)
[info] pmm: 32 node(s) own memory
```

The second line is false, and the third proves it: 32 nodes were established from the SRAT the first line was reading. `numa: the ACPI SRAT places N CPU(s)` never appears, and `pmnode.test` reports `0 of 8 possible CPU(s) were placed by firmware` -- against `8 of 8` on the four-node machine, whose firmware does carry a SLIT.

**Cause**
`register_nodedis()` returns `Status::NotFound` when there is no SLIT. That return value is correct about the SLIT and wrong as a verdict on ACPI discovery, because `acpi_init_numa()` treats it as a failure:

```cpp
    status = register_nodedis(*tables);
    if (status != Status::Ok) {
        // Already said which way it failed, and said it with the reason.
        return status;
    }
```

Two consequences follow from that one early return. `bind_node_to_cpu()` never runs, so `Topology::set_firmware_node()` is never called, every entry of the firmware placement table keeps its sentinel, and `PmNode::init()` builds each node's hard affinity empty. And `x86_init_numa()`, seeing a non-Ok status from the ACPI probe, falls through to the PCI fallback and then logs `no firmware topology` -- the line whose honesty was the whole point of `7814aa93`. The memory affinities are not rolled back on the way out, so the kernel goes on to bring up every node the SRAT named while the log says it found none.

A missing SLIT is not a failure to discover the topology. It is a machine that named its proximity domains and not their distances; the distances then degenerate to node-id order, which `register_nodedis()` already logs and already handles by zeroing the matrix.

**Change**
Separate "there is no SLIT" from "ACPI discovery failed". `register_nodedis()` returning `Status::NotFound` for a missing table is fine, but `acpi_init_numa()` must not treat it as fatal: carry on to `bind_node_to_cpu()` and return `Ok`, because the SRAT was read and applied. A malformed or rejected SLIT is a different matter and may keep its current treatment -- decide that explicitly rather than by reusing one status for both.

Note the ordering trap: `register_nodedis()` zeroes the whole matrix before returning `NotFound`, which is the behaviour the fallback order needs, so the early return must be removed without removing that reset.

**Reference**
Linux's `acpi_numa_init()` parses the SRAT and then calls `acpi_table_parse(ACPI_SIG_SLIT, acpi_parse_slit)` without looking at the result: a missing SLIT leaves `node_distance()` on its defaults (`LOCAL_DISTANCE` / `REMOTE_DISTANCE`) and the topology stands. Only SRAT failure reaches `bad_srat()`.

**Acceptance**
On `NUMA=33`: `numa: the ACPI SRAT places 8 CPU(s)` appears, `pmnode.test` reports `8 of 8 possible CPU(s) were placed by firmware`, and no line claims there was no firmware topology. `NUMA=1`, whose firmware does carry a SLIT, is unchanged.
