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

## The fallback order is derived outside mm, and the allocator stops reading distances (2026-10-29)

**Files**
- `kernel/lib/numa/include/numa/fallback_order.hpp`, `kernel/lib/numa/src/fallback_order.cpp` — new: `build_fallback_orders()`, the derivation itself
- `kernel/lib/numa/test/fallback_order_test.cpp` — new: eight host cases over matrices the test writes itself
- `kernel/lib/numa/BUILD.gn`, `kernel/lib/numa/test/BUILD.gn`, `BUILD.gn` — the source, and the tests through a group
- `kernel/mm/include/ours/mm/pm_node.hpp` — `PmNode::Router` deleted, 56 lines; `connect()` retyped; `node_memory_mask()` added
- `kernel/mm/pm_node.cpp` — `Router::build()` gone, `connect()` appends what it was handed
- `kernel/mm/pm_node_init.cpp` — one call for the whole machine
- `kernel/commands/pm_node_test.cpp` — checks the derivation against this machine's masks

**Symptom**
None observable. The same order, computed by something that is allowed to know what a distance is.

**Cause**
Step 4 of `.agents/tasks/numa-out-of-mm/PLAN.md`, the cut the whole task exists for.

`ZoneList::connect(NodeMask const &)` took a set of nodes and chained their zones behind this node's own. To do that it had to put them in order, so inside it called `PmNode::Router::build(m_nid, nodemask)`, which read `numa::Topology::distance()`. That was the last reader of the distance matrix in mm, and it sat inside the allocator's own data structure.

The signature is where the problem shows: one argument said *which* nodes, and the function also decided *in what order*, invisibly. A caller could not ask for one without the other, and the ordering rule -- a NUMA concept, the only thing the matrix exists for -- was a private implementation detail of a list of zone pointers.

**Change**
The derivation is `numa::build_fallback_orders()`, and `connect()` takes the answer:

```cpp
auto connect(ustl::views::Span<NodeId const> order) -> void;
```

A span of node ids rather than the library's own type, so that mm's header names nothing from `numa/`: what arrives is "an ordered list of node ids", which is a statement mm can make in its own vocabulary.

The derivation takes two sets, which is the one place this change is not a straight move:

```cpp
auto build_fallback_orders(NodeMask const &nodes, NodeMask const &members,
                           FallbackOrders *orders) -> void;
```

`nodes` is who needs a row, `members` is who may be a hop. They were one argument before -- `Router::build(from, candidates)` took the candidate mask and walked the `Memory` state inside it, so the restriction happened in two places at once. Collapsing them into one set here was the first thing written, and it was wrong: a node that owns no memory would have got no fallback order at all, and that is the one node that cannot do without one, since every allocation on it has to go somewhere else. The host test now has that case by name.

The whole table comes back through an out-parameter and lives at file scope in `pm_node_init.cpp`, because on a NUMA build it is `MAX_NODES` rows of up to `MAX_NODES - 1` node ids -- about four kilobytes -- and `bootstrap()` runs on a bring-up kernel stack. It is dead the moment `connect_nodes()` returns, so it costs .bss on a NUMA build and nothing at all where `MAX_NODES` is 1.

One call for the whole machine, which was the user's decision when asked: the derivation is the same scan either way, and asking once is what keeps the distance matrix to a single reader, at a single moment. The alternative considered and rejected was a streamed form -- a callback per node, one row live at a time, no table anywhere -- which costs no .bss but puts the derivation's loop in a header as a template.

`Router::for_each_node()` went with the class. Its only caller was the test that checked it.

**Reference**
Linux builds the same thing in the same place: `build_zonelists()` in `mm/page_alloc.c` calls `find_next_best_node()`, which scans `node_distance()` for the nearest unvisited node, and fills each node's `zonelist` with the result. The derivation is a function over the distance table and the zonelist is what it fills -- not a method on the zonelist that reaches for the table. `numa_init()` owns the matrix; the allocator owns the list.

**Verification**
```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  151 passed, 0 failed
```

151 rather than 150: the new host test arrives through `//kernel/lib/numa/test:tests`, a group, so the next one added there runs without touching the root. `pmnode.test: ok -- 20 checks` (21 before, one fewer because `for_each_node()` is gone) and `pmzone.test: ok -- 3 checks`.

The eight host cases are the real evidence, and they check things the kernel could not: nearest-first over a matrix the test fills, each row ordered from its own node rather than one order shared, a node listed exactly once, a node never a hop of its own, and the two-set case above. `git grep 'Topology::distance' -- kernel/mm` is now empty.

**Deliberately not done**
The plan's own check for this step -- `ZoneList::dump_fallback()`'s boot output byte-identical before and after -- was not performed, because it cannot be, and saying so is better than quietly substituting something weaker. Two reasons, either sufficient: the dump goes to `log::debug` from `bootstrap()`, which is early enough that nothing reaches the serial log, so neither run prints it; and the default build has `MAX_NODES == 1`, so a node's fallback order has no remote hops in it and the two implementations are indistinguishable by construction.

What was done instead was to go after the condition that made it vacuous: build with `mm_numa = true` and boot the four-node QEMU topology. That took two fixes to get as far as a boot (`fix(x86)`, the entry above) and then showed that the kernel brings up one node anyway -- `docs/repairs/261029/mm.md` item 1. So this step's equivalence rests on the host test and on the algorithm having moved line for line, not on a multi-node boot, and the repair item is what has to land before a multi-node boot can check anything.

**How it was found**
A planned step. Two things worth keeping came out of doing it.

The first is the two-set signature, above: a single-set version passes every test on a one-node machine and silently strands a memory-less node. It was caught by writing out what `Router::build`'s two arguments had each been for, not by a test -- there was no test that could fail.

The second is that this step's verification criterion was unachievable as written, which is the second plan step in a row where the plan said something that was not so (step 3's was a claim about where `apply_thread_policy()` already lived). Both were written before the code was read closely. The lesson is narrow and worth stating: a verification criterion in a plan is a hypothesis about the code, and it deserves checking at the moment it is written, since it is the one line of a plan that is supposed to be the backstop for all the others.

**The user's prediction**
None -- homework is paused. The handoff shape was the user's decision: the whole table, taken in one call, over a per-node query.

**Three questions you should be able to answer**
1. `connect()` used to take one `NodeMask` and now takes an ordered span. Say what the old signature made impossible to ask for, and why that is a design problem rather than an inconvenience.
2. A single-set `build_fallback_orders()` passes every test in this tree. Name the machine it breaks on and the node on it that suffers.
3. The plan asked for byte-identical `dump_fallback()` output. Give both independent reasons that check could not work, and say what you would build so that it could.
