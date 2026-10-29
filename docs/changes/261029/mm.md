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

## A NUMA kernel on four nodes was working all along, and three conclusions about it were wrong (2026-10-29)

**Files**
- `.agents/skills/ours-verify/SKILL.md` — the boot logs are not text, so plain `grep` lies about them
- `docs/repairs/261029/mm.md` — item 1 replaced: the symptom it was filed for was a mis-measurement, the real hole beside it is the silence
- `.agents/tasks/numa-out-of-mm/PLAN.md` — step 4's verification criterion was achievable after all, and has now been met

**Symptom**
Three claims in the two entries above this one are false. Stated plainly, because they are committed and someone will read them:

1. "The NUMA kernel still brings up one node on a four-node machine." It does not. It brings up four.
2. "`ZoneList::dump_fallback()`'s output never reaches the serial log." It does, at `log::debug`, from `bootstrap()`.
3. "Step 4's byte-identical check cannot be performed." It can, and it has been.

**Cause**
Two independent mistakes, one of which hid the other.

The first is a tool that fails silently. These boot logs contain a handful of NUL bytes — three, in a 682 KB log — because the serial capture is raw device output. BSD `grep` classifies such a file as binary and then reports **no matches at all**: no output, and exit status 1. Not "Binary file matches", not a count of zero. `grep -c 'CPU' dbg.log` on a log containing thousands of `CPU` lines prints nothing and exits 1, which is indistinguishable from a log that genuinely does not contain the string.

Every conclusion I drew about these logs came through that. `grep -iE 'numa|srat|slit|node'` over the four-node boot log returned nothing, so I concluded the NUMA path was inert and the fallback dump was unreachable. Read with Python, the same file says:

```text
  70  [1756129000 trace] NUMA information from ACPI-SRAT
 107  [1766316000 debug] Node[0-fallback]
 113  [1766699000 debug] Node[1-fallback]
 119  [1766938000 debug] Node[2-fallback]
 125  [1767189000 debug] Node[3-fallback]
```

The second is mine and simpler. The numbers in the retracted repair item — `1 node(s) online`, `0 of 4 possible CPU(s) were placed by firmware` — came from this pair of commands:

```bash
NUMA=1 python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/numa.x86 --no-tests
python3 scripts/kshell_run.py --out out/numa.x86 pmnode.test
```

`NUMA=1` is on the first line and not the second. `mm_numa` is a GN argument: it sets `MAX_NODES` and compiles `srat.cpp`, and changes not one byte of QEMU's command line. The topology comes from the environment variable alone (`scripts/qemu_boot.py`, `NUMA = os.environ.get("NUMA", "0") != "0"`), and `kshell_run.py` passes the environment through rather than deriving it from the out directory. So the second command booted the NUMA *build* on a uniform four-CPU machine, where one node online and no firmware-placed CPUs is the correct answer. I read the right number off the wrong machine, and the broken `grep` meant the log could not contradict me.

What makes this worth a full entry rather than a line: the two mistakes compose into a false positive that looks like evidence. A silent tool failure returns "absence", a mis-measurement supplies a plausible symptom for that absence, and the result is a diagnosis with a reproduction case attached. It was filed as a Medium-severity repair item with a three-stage investigation plan for a bug that does not exist.

**Change**
`ours-verify`'s gotchas now say that the boot logs are not text, that plain `grep` silently reports no matches on them, and that `grep -a` or a Python read is required. That is the fix that matters, because the trap is in the tool and will otherwise catch the next reader exactly as it caught this one.

The repair item is re-filed. What the five readers found that *did* survive being attacked is a real hole in the same area, and it is the reason the mis-measurement was so easy to believe: a kernel that finds no NUMA topology says nothing about it. `x86_init_numa()` tests `acpi_init_numa()`'s status, discards it, and logs nothing on any path; three exits in `srat.cpp` return silently. "This machine declares no NUMA" and "the parse failed" produce byte-identical output, which is nothing. That is item 1 now.

Step 4's criterion has been met. The baseline, `9b74b07a`, was built in a worktree of its own and booted on the four-node machine; so was `2994f6b4`; the `Node[*-fallback]` blocks are 24 lines each and `diff` reports them identical. The orders are genuinely distance-ordered and differ per node — node 1 falls back `1, 0, 3, 2`, node 2 `2, 3, 0, 1`, node 3 `3, 2, 1, 0` — so the derivation is exercised rather than degenerating to node-id order, which is what makes the comparison mean something.

Rejected: quietly amending the two entries above. They are wrong and the record is append-only, and a reader who finds the retraction beside the claim learns more than one who finds a clean story.

**Reference**
None for the grep behaviour — it is POSIX-permitted and BSD-specific. GNU `grep` prints `Binary file ... matches` instead, which is at least visible.

**Verification**
```text
NUMA=1 kshell_run.py --out out/numa.x86 pmnode.test
  pmnode.test:   cpu0 is on node 0; 4 node(s) online
  pmnode.test:   8 of 8 possible CPU(s) were placed by firmware
  pmnode.test: ok
```

```text
diff fb_baseline.txt fb_step4.txt   # 9b74b07a vs 2994f6b4, NUMA=1, 24 lines each
  (no output)
```

`grep -ac 'Node\[' dbg.log` answers 4; without `-a` it answers nothing and exits 1.

**Deliberately not done**
`NUMA=1` is not added to the standard verification, and the topology is not printed in the boot log header. Both are good ideas — the second would have prevented this outright — and both change what `ours-verify` does for every change in the tree, which is the user's call rather than mine. Raised instead of done.

The instrumentation for repair item 1 is not written either. It is that item.

**How it was found**
By not believing a diagnosis. The repair item was filed and reported, and then the same question was handed to five independent readers with the brief "establish which stage fails, cite file:line, and a clean bill of health is as useful as a bug". Each proposed candidate was then attacked by two skeptics told to refute it, one on the code reading and one on whether the claimed cause could actually produce the observed symptom.

That second lens is what broke it open. Asked whether a silent SRAT parse could produce "boots cleanly, one node, zero firmware-placed CPUs", a reader went to the logs instead of the source, found two logs from the same binary one minute apart — one with `-smp 8` and four `Node[*-fallback]` blocks, one with `-smp 4` and none — and worked backwards to the missing environment variable. No amount of reading `srat.cpp` would have got there, because `srat.cpp` was never the problem.

The lesson is about the shape of the mistake rather than about NUMA. A tool that fails by returning nothing is worse than one that fails loudly, and the way it fails — absence of evidence arriving as evidence of absence — is exactly the shape that a plausible second mistake completes into a finished-looking story. Two cheap habits would each have caught it on its own: checking that a grep over a large log finds *something* before trusting that it finds nothing, and putting the machine's shape in the log so that a number can be traced to the machine it came from.

**The user's prediction**
None — homework is paused.

**Three questions you should be able to answer**
1. `grep -c 'CPU' dbg.log` prints nothing and exits 1 on a log full of `CPU` lines. Say why, and name the flag that fixes it.
2. `mm_numa = true` and `NUMA=1` are both needed for a four-node boot, and they act at different times on different things. Say what each one does, and what you get from either one alone.
3. Five readers were given this problem and the answer came from the one attacking causality rather than code. State the question that lens asks, and why reading `srat.cpp` more carefully could never have answered it.
