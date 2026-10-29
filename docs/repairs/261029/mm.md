# mm outstanding repairs

Both items were raised by a review of the change that filled in the NUMA bring-up's silence, confirmed in full by a skeptic, and then refused as defects of *that* change because they predate it and it did not touch them. They are real; they are just somebody else's commit. See `docs/changes/261029/mm.md`.

## 1. A SLIT declaring more than MAX_NODES localities writes past the distance matrix

**Severity**
High. It is an out-of-bounds write driven by firmware data, with no check at all in the image this project ships. Not reachable on either machine the project boots today, which is the only reason it is not Critical.

**Symptom**
Latent, cannot be triggered today: both QEMU machines `scripts/qemu_boot.py` builds declare four localities, and the default build has `MAX_NODES == 1` with the whole path compiled out. It would be triggered by real hardware, or a hand-written QEMU topology, declaring more than 32 proximity domains in its SLIT — which large multi-socket machines do.

The write lands in `numa::Topology::DISTANCE`, a `MAX_NODES` × `MAX_NODES` array of `usize` — 8 KiB of `.bss` under `mm_numa` — and goes past its end by as much as the firmware's count squared.

**Cause**
Three bounds that each check something else, and none of them checks this one.

`register_nodedis()` loops over the firmware's own count:

```cpp
auto const count = matrix->count();
for (auto from = u32(0); from < count; ++from) {
    for (auto to = u32(0); to < count; ++to) {
        ::numa::Topology::set_distance(from, to, matrix->distance(from, to));
    }
}
```

`DistanceMatrix::from_table()` validates `count` against the SLIT's payload length — a count whose square does not fit is rejected — and against `u32`. It never compares it with `MAX_NODES`, and it has no reason to: `kernel/lib/acpi` describes ACPI tables and knows nothing about how many nodes this kernel can represent. `DistanceMatrix::distance()` returns 0 for an out-of-range read, so the read side is safe.

`Topology::set_distance()` is the write side, and its only guard is `DEBUG_ASSERT(x < MAX_NODES && y < MAX_NODES)`. `DEBUG_ASSERT` is `if constexpr (KERNEL_DEBUG_LEVEL)`, so it compiles to nothing at level 0 — and the `dev` image, the one that boots by default, is built at level 0. Only `dev.dbg_1` would catch it, and then as an assertion failure rather than a diagnosis.

`NodeId` is `u32` and the loop indices are `u32`, so no narrowing accidentally saves it.

So the firmware's declared locality count crosses from a library that is right not to know `MAX_NODES` into a kernel that does, and nobody clamps it on the way.

**Change**
Clamp where the two vocabularies meet, which is `register_nodedis()` — the one place that holds both the firmware's count and the kernel's capacity:

```cpp
auto const declared = matrix->count();
auto const count = ustl::algorithms::min<usize>(declared, MAX_NODES);
if (count < declared) {
    log::warn("numa: the SLIT declares {} localities, using {}", declared, count);
}
```

The log line is the point as much as the clamp: a machine whose topology is silently truncated to the first 32 nodes should say so, or the fallback orders will look merely odd.

Rejected: making `set_distance()` refuse out-of-range writes instead of asserting. Worth doing as well, but it cannot be the fix on its own — it would turn a corruption into a silent discard, and the caller would still believe it had recorded a 64-node topology.

Rejected: raising `MAX_NODES`. It is 32 because that is what the node masks and per-node arrays are sized for; the bug is the missing clamp, not the number.

**Reference**
Linux clamps at exactly this seam. `acpi_parse_slit()` in `arch/x86/mm/srat.c` calls `bad_srat()` and refuses the table when `slit->locality_count` exceeds `MAX_NUMNODES`, before `numa_set_distance()` is ever reached, and `numa_set_distance()` itself range-checks and `pr_warn`s rather than writing: `"NUMA: Warning: node ids are out of bound, from=%d to=%d distance=%d\n"`.

**Acceptance**
A kernel built with `mm_numa = true`, booted on a QEMU machine hand-configured with more than 32 nodes, must boot, must log that the SLIT was truncated, and must not corrupt memory. `//kernel/lib/numa/test:fallback_order` is the natural place for a host case asserting that a count above `MAX_NODES` is clamped, since the derivation is already tested there.

## 2. register_meminfo() reports success having re-homed only some memory regions, or none

**Severity**
Medium. Wrong, and reachable on any machine whose SRAT declares a domain id this kernel cannot represent, but with no consequence worse than a node's memory landing on the wrong node.

**Symptom**
Latent on the machines booted today: QEMU's four domains are all below `MAX_NODES`, so every region is re-homed and the function's return value is honest. On a machine with more proximity domains than `MAX_NODES`, or with a malformed domain id, some regions keep node 0 while others move, and `acpi_init_numa()` is told `Status::Ok`.

**Cause**
The walk continues after the first failure, and the per-region work is skipped rather than the walk abandoned:

```cpp
if (assignment_status == Status::Ok && region.domain < MAX_NODES) {
    assignment_status = mm::EarlyMem::set_node(...);
    ...
}
```

Two different conditions share one guard. A `region.domain >= MAX_NODES` silently skips that region and leaves `assignment_status` untouched — so the function returns `Ok` having ignored it — while a failing `set_node()` latches the status and makes every later region be skipped too. Neither case is logged, and the caller cannot tell a complete re-homing from a partial one.

The comment above it says the skip is deliberate, for machines that are not NUMA. That reasoning covers a domain id of 0 on a uniform machine; it does not cover a domain this kernel cannot represent, which is a truncated topology and should be reported as one.

**Change**
Separate the two conditions and count them. A region whose domain is beyond `MAX_NODES` is a truncation: count those and log the total once after the walk, which is also what item 1's clamp should report. A failing `set_node()` is a real error and should end the walk rather than quietly skipping the remainder.

Then decide what the function returns for a partial success, and say it in the signature's documentation — the present `Status` cannot express "some". Returning the count of regions re-homed, or taking an out-parameter for it, would let the caller log the truth.

**Reference**
Linux's `acpi_numa_memory_affinity_init()` in `arch/x86/mm/srat.c` rejects the whole table through `bad_srat()` when it cannot use an entry, rather than applying part of it — `numa_init()` then falls back to one node over all memory. Partial application is the case it refuses to create.

**Acceptance**
A host or kshell case over an SRAT whose domain ids exceed `MAX_NODES`: the boot must log how many regions were left behind, and the status `acpi_init_numa()` sees must not claim success for a partial application.
