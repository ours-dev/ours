# mm outstanding repairs

Raised by a review of the change that filled in the NUMA bring-up's silence, confirmed in full by a skeptic, and then refused as a defect of *that* change because it predates it and it did not touch it. The other item filed alongside it — a SLIT's locality count reaching the distance matrix unclamped — was fixed on 2026-10-29; see `docs/changes/261029/mm.md`.

## 1. register_meminfo() reports success having re-homed only some memory regions, or none

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
