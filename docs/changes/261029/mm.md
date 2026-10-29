# mm changes

## The NUMA build can be configured again, and srat.cpp compiles (2026-10-29)

**Files**
- `kernel/arch/x86/mm/BUILD.gn` — the NUMA branch appends to `deps` instead of assigning over it
- `kernel/arch/x86/mm/srat.cpp` — `::numa::Topology` for the library's new home, two sign-compare fixes, and a comment naming functions that exist

**Symptom**
`mm_numa = true` could not be configured at all, so no NUMA kernel had been built since `ad246ae5`:

```text
ERROR at //kernel/arch/x86/mm/BUILD.gn:26:5: Replacing nonempty list.
    deps = [ "//kernel/lib/sampler:request" ]
    ^---
See //kernel/arch/x86/mm/BUILD.gn:22:14: for previous definition
      deps = [ "//kernel/lib/numa" ]
```

Past that, `srat.cpp` did not compile: three uses of `mm::numa::Topology`, a namespace that does not exist, and two `-Wsign-compare` errors in `reset_all_distances()`.

**Cause**
Both are the same mistake with the same cause: a file that nothing builds is a file nothing checks.

The GN error is mine, from step 2 of `.agents/tasks/numa-out-of-mm/PLAN.md`. Adding `deps = [ "//kernel/lib/numa" ]` inside `if (mm_numa || mm_numa_emulation)` put an assignment before the unconditional `deps = [ "//kernel/lib/sampler:request" ]` a few lines below, and GN refuses to assign over a nonempty list rather than silently dropping one. The default build never noticed, because with `mm_numa = false` the branch does not run and the single remaining assignment is fine.

That failure is what hid the next one. `srat.cpp` is compiled only under `mm_numa || mm_numa_emulation`, so when step 2 moved `NumaTopology` out of `ours::mm` into the free-standing `numa` namespace and renamed `set_cpu_node_early()` to `set_firmware_node()`, this file was edited by the same sweep as every other and then never compiled. `mm::numa::Topology` is what the sweep produced -- the old `mm::` qualifier with the new namespace behind it -- and it is exactly the kind of error a compiler catches in a second and a text search does not.

The two sign-compare errors are older than either: `for (auto i = 0; ...)` gives an `int`, and `MAX_NODES` is a `usize`. Unreachable before this because the file was already unbuildable, and `-Wconversion`/`-Wsign-compare` are errors in this tree.

**Change**
`deps +=` in the branch, and moved below the unconditional assignment so that the order reads the way it has to work. The comment says what went wrong, because the next person adding a conditional dependency here will be one line away from repeating it.

In `srat.cpp`, `::numa::Topology` rather than `numa::Topology`: this file is inside `namespace ours` and already uses `acpi::numa::` a few lines away, so the leading `::` says which `numa` is meant without the reader having to work it out. `NodeId(0)` for the loop counters. And the comment above `set_firmware_node()` now names `set_firmware_node()` and `PmNode::set_cpu_node()`, the functions that exist, rather than `set_cpu_node_early()` and `set_nid()`, which do not.

Rejected: a `mm_numa` build in CI, or in the default verification. It is the right answer and it is not this change -- the NUMA kernel does not yet bring up more than one node (`docs/repairs/261029/mm.md`, item 1), so adding it now would lock in a boot that passes by having nothing to do.

**Reference**
None, this project's own build.

**Verification**
```bash
python3 tools/set.py bringup.x86 --out out/numa.x86 --args 'mm_numa=true'   # configures
prebuilt/ninja/ninja -C out/numa.x86                                        # builds
NUMA=1 python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/numa.x86
```

```text
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
```

The default build is untouched by either file at `mm_numa = false`, and its own verification is in the entry below, which was in the tree at the same time: these two were found while verifying it.

**Deliberately not done**
The NUMA kernel still sees one node on a four-node machine. Filed as `docs/repairs/261029/mm.md` item 1 rather than chased here; this change is what made it possible to observe at all.

**How it was found**
Step 4 of the plan asks for proof that moving the fallback-order derivation did not change the order, and the plan's own criterion for that was `ZoneList::dump_fallback()`'s boot output being byte-identical. That turned out to be unobtainable twice over -- the dump goes to `log::debug` at a point in bring-up where nothing reaches the serial log, and on a one-node machine there are no remote hops in it to compare -- so the way to make the check mean anything was to boot a machine with more than one node. Trying to configure that build is where this stopped.

**The user's prediction**
None -- homework is paused.

**Three questions you should be able to answer**
1. The default build compiled this `BUILD.gn` every time and never complained. Say exactly why, in terms of which statements GN executes.
2. A tree-wide rename touched `srat.cpp` correctly everywhere except the namespace qualifier. Name the property of this file that let that survive, and say what would have caught it.
3. `mm_numa = true` is not added to the verification script in this change. Give the reason, and say what has to be true before it should be.
