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

## The NUMA bring-up says what it found, and the node count is reported where it is a fact (2026-10-29)

**Files**
- `kernel/arch/x86/mm/numa.cpp` — `x86_init_numa()` reports the fallback instead of discarding two statuses; gains `<logz4/log.hpp>`
- `kernel/arch/x86/mm/srat.cpp` — five silent returns, two SLIT failures sharing one string, and a walk that logged nothing all get their own line
- `kernel/mm/pm_node_init.cpp` — `bootstrap()` logs how many nodes own memory; gains `<logz4/log.hpp>`
- `docs/repairs/261029/mm.md` — item 1 deleted, fixed

**Symptom**
A kernel that found no NUMA topology, or found one it could not use, said nothing whatsoever. Both outcomes produced byte-identical boot output — none — and so did success on a single-node machine. On 2026-10-29 that silence contributed to a correct one-node boot being filed as a kernel bug.

**Cause**
`docs/repairs/261029/mm.md` item 1. Nine places, all the same shape: a status known at the moment it was thrown away.

`x86_init_numa()` tested both sources' statuses and discarded them, with no log statement on any path — the file included no logging header at all. In `acpi_init_numa()`, five returns were silent: no ACPI tables, no SRAT, unusable memory affinities, and the two later stages' failures. `register_nodedis()` printed the bare string `"No NUMA"` for two unrelated causes — a missing SLIT and a rejected matrix — and returned `Status::Ok` with no output at all when the SLIT declared zero localities, which leaves every distance zero. `bind_node_to_cpu()` logged nothing and threw away its binding count, so a walk that recorded no CPUs left no trace. And `print_numa_info()` used `"No NUMA"` a third time, for a malformed memory-region walk.

**Change**
Every one of those says what it found, with the reason, and the two `"No NUMA"` strings are gone. The messages are deliberately short: a formatted body over 256 bytes panics the kernel rather than truncating (`docs/repairs/261028/logz4.md` item 1), and the worst case here is about 102 bytes — the fallback line's literal plus two of the longest `Status` names.

The node count is the part worth dwelling on, because the first draft of this change got it wrong in exactly the way the change exists to prevent.

That draft had `x86_init_numa()` print `no firmware topology (...); one node covers all memory`. The second clause is false on four of the six states this path can reach. `acpi_init_numa()` applies the SRAT's memory affinities **first** — `register_meminfo()` tags every region with `EarlyMem::set_node()` and marks each domain `Possible` — and only then reads the SLIT and the CPU bindings. Nothing rolls those tags back, and `find_possible_nodes()` derives the node set straight from them. So on a machine with an SRAT and no SLIT, which is both legal and common, `register_nodedis()` returns `NotFound`, the fallback line fires, and four nodes come up underneath a line claiming one. On the dbg images the following `Node[0..3-fallback]` dump contradicts it; on rel that dump is `log::debug` and compiled out, so the false line is the only thing a reader gets.

So `numa.cpp` now states only what it knows at that point — which sources declined, and with what status — and the count is reported from `PmNode::bootstrap()`, after `find_possible_nodes()`, where it is a fact:

```cpp
log::info("pmm: {} node(s) own memory", nodes.count());
```

Unconditional, on every boot of every build, because the interesting case is the one where it reads 1 unexpectedly. It is also the only line on the subject that a release image keeps.

Rejected: rolling back the memory affinities when a later stage fails, so that the early claim would become true. It throws away good information — the domains the firmware declared are correct whether or not the SLIT parses — to make a log line accurate, which is backwards.

Rejected: asserting rather than logging on a missing SRAT. A machine without one is an ordinary machine, and almost every boot in this project is exactly that.

**Reference**
Linux's `numa_init()` in `arch/x86/mm/numa.c` falls back to `dummy_numa_init()` when any stage fails, and that function announces itself — `pr_info("Faking a node at [mem %#018Lx-%#018Lx]\n", ...)`. The split this change arrives at is also Linux's: `numa_init()` reports which source it took, and the node count comes from the allocator's own bring-up, not from the firmware parse. A parse cannot report a consequence that later code decides.

**Verification**
Four boots, both machines times both images, with the logs kept rather than overwritten.

```text
uniform-memory machine (NUMA unset)        four-node machine (NUMA=1)
  dbg  PASS  44 passed, 4 skipped, 0 failed   dbg  PASS  44 passed, 4 skipped, 0 failed
  rel  PASS  41 passed, 8 skipped, 6/6 req    rel  PASS  41 passed, 8 skipped, 6/6 req
```

```text
uniform, dbg and rel alike:
  numa: no ACPI SRAT; the machine declares no proximity domains
  numa: no firmware topology (acpi NotFound, pci NotSupported)
  pmm: 1 node(s) own memory

four-node, dbg and rel alike:
  numa: the ACPI SLIT declares 4 localities
  numa: the ACPI SRAT places 8 CPU(s)
  pmm: 4 node(s) own memory
```

The two machines are now distinguishable from the log alone, on both images, which is what the repair item asked for. The default build — where `numa.cpp` and `srat.cpp` are not compiled at all — carries `pmm: 1 node(s) own memory` and nothing else, and its own verification is unchanged:

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  151 passed, 0 failed
pmnode.test: ok -- 20 checks · pmzone.test: ok -- 3 checks
```

On the four-node machine `pmnode.test` reports **56 checks** against 20 on the uniform one, because its per-node cases run once per node. That ratio is the argument for booting it.

**Deliberately not done**
Two weaknesses that three reviewers raised and that are real, both pre-existing and neither touched here, filed as items 1 and 2 of `docs/repairs/261029/mm.md`: the SLIT fan-out loop is bounded by the firmware's locality count rather than by `MAX_NODES`, with only a `DEBUG_ASSERT` behind it; and `register_meminfo()` can return `Status::Ok` having re-homed only some of the SRAT's regions, or none.

`print_numa_info()` and `register_meminfo()` both walk every memory region printing a line, so the same table is dumped twice per boot. Noted, not fixed.

**How it was found**
The item was written, then the change was written, then the change was reviewed by three agents on separate lenses — completeness, logging hazards, and a reader test — with every finding handed to a skeptic told to refute it. Fifteen findings, thirteen refuted.

One of the two survivors is the false `one node covers all memory` line above, found by the reader lens, which did the thing an author does not: it enumerated the six states this path can reach, wrote down the exact lines each would print, and checked each message against what the code actually does next rather than against what the message says.

The other survivor was about the verification rather than the code: the uniform-memory output quoted as evidence was in no log on disk, and rel had never been booted with the change at all. Both halves were right. Two boots had used the same out directory, so the second overwrote the first's log, and both had been `--only dbg`. The claim that those branches had therefore never executed was wrong — they had, and the output was read before it was overwritten — but that is not a defence. Evidence that no longer exists is not evidence, and the fix is the one above: four boots, logs kept.

Worth keeping from the thirteen refutations: every one confirmed its finding's mechanism in full and then rejected it on scope or reachability. That is the useful shape for a review of a small change — "true, and not a defect of this diff" is a different verdict from "false", and collapsing the two either inflates the change or loses the finding. Both of the high-severity ones that were refuted came back as repair items instead.

**The user's prediction**
None — homework is paused.

**Three questions you should be able to answer**
1. `one node covers all memory` is false on an SRAT-without-SLIT machine. Trace the order of operations inside `acpi_init_numa()` that makes it false, and say which function has already committed the nodes by then.
2. The node count is logged from `PmNode::bootstrap()` rather than from `x86_init_numa()`. Give two reasons, one about correctness and one about what a release image keeps.
3. Thirteen of fifteen review findings were refuted, and two of those were high severity and went on to become repair items. Say what distinguishes "refuted" from "false" here, and why collapsing them would have been worse in both directions.

## A firmware-declared locality count can no longer write past the distance matrix (2026-10-29)

**Files**
- `kernel/arch/x86/mm/srat.cpp` — the SLIT fan-out is bounded by `MAX_NODES` as well as by the firmware's count, and says when it truncates; gains `<algorithm>`
- `kernel/lib/acpi/test/numa_test.cpp` — a host case pinning the premise the clamp rests on
- `docs/repairs/261029/mm.md` — item 1 deleted, fixed

**Symptom**
Latent, cannot be triggered on any machine this project boots: both QEMU topologies declare four localities, and the default build has `MAX_NODES == 1` with the whole path compiled out. A SLIT declaring more than `MAX_NODES` localities — which large multi-socket hardware does — wrote past the end of `numa::Topology::DISTANCE`, an 8 KiB array under `mm_numa`, by as much as the declared count squared.

**Cause**
`docs/repairs/261029/mm.md` item 1, raised by a review of the change before this one and confirmed in full before being refused as a defect of *that* change.

Three bounds, each checking something real, and none of them checking this.

`register_nodedis()` looped over the firmware's own count straight into the setter. `acpi::numa::DistanceMatrix::from_table()` validates that count against the SLIT's payload length — a count whose square does not fit is rejected — and against `u32`, but never against `MAX_NODES`, and it is right not to: `kernel/lib/acpi` describes ACPI tables, not this kernel's capacity. `DistanceMatrix::distance()` clamps out-of-range reads to 0, so the read side was already safe. The write side is `Topology::set_distance()`, whose only guard is `DEBUG_ASSERT(x < MAX_NODES && y < MAX_NODES)` — and `DEBUG_ASSERT` is `if constexpr (KERNEL_DEBUG_LEVEL)`, which compiles to nothing at level 0. The `dev` image is level 0; only `dev.dbg_1` would have caught it, as an assertion failure rather than a diagnosis.

`NodeId` is `u32` and the loop indices were `u32`, so no accidental narrowing saved it.

So the firmware's count crossed from a library that is right not to know `MAX_NODES` into a kernel that does, and nobody clamped it on the way. The seam had no owner.

**Change**
The clamp goes where the two vocabularies meet, which is the one place holding both the firmware's count and the kernel's capacity:

```cpp
auto const usable = u32(std::min<usize>(declared, MAX_NODES));
if (usable < declared) {
    log::warn("numa: using {} of them; this kernel holds {} nodes", usable, MAX_NODES);
}
```

The warning is as much the point as the bound. A truncated topology produces fallback orders with nodes missing from them, and a reader puzzling over those has to be able to find the reason; `log::warn` rather than `info` because unlike every other line on this path it describes something the machine wanted and did not get.

**This truncates where Linux refuses, which is a deliberate departure and the one thing here worth disagreeing with.** `acpi_parse_slit()` in `arch/x86/mm/srat.c` calls `bad_srat()` when `slit->locality_count` exceeds `MAX_NUMNODES`, rejecting the table outright, and `numa_init()` then falls back to `dummy_numa_init()` — one node over all memory. The reason for not following it: the distances we keep are *correct*. `DistanceMatrix::distance(from, to)` indexes with the **declared** stride, so the leading `usable`-by-`usable` square of the payload holds those nodes' true mutual distances rather than a reshaped slice. Refusing the table would discard that and leave the nodes we do have in node-id order, which is strictly worse for them. Linux's choice makes more sense in its context, where `numa_register_memblks()` would otherwise have to reconcile a node set and a distance matrix that disagree; here `register_meminfo()` has already skipped the domains above `MAX_NODES`, so the node set and the truncated matrix agree by construction.

That reasoning is worth checking rather than taking: if refusing is preferred, the change is to return an error from `register_nodedis()` instead of clamping, and the warning already names the number that decides it.

Rejected: making `set_distance()` refuse out-of-range writes rather than asserting. It is worth doing, and it cannot be the fix — it would turn a corruption into a silent discard while the caller went on believing it had recorded a 64-node topology. The clamp is what makes the caller's belief true.

Rejected: raising `MAX_NODES`. It is 32 because that is what the node masks and the per-node arrays are sized for. The defect is the missing clamp, not the number.

**Reference**
Linux, `arch/x86/mm/srat.c`: `acpi_parse_slit()` rejects a `locality_count` above `MAX_NUMNODES` through `bad_srat()`, and `numa_set_distance()` in `arch/x86/mm/numa.c` range-checks before writing and warns rather than scribbling — `"NUMA: Warning: node ids are out of bound, from=%d to=%d distance=%d\n"`. The principle followed here is that second one: the bound belongs on the path to the array, and exceeding it is a warning rather than a silent write. The departure is at the first: this truncates the table where Linux discards it, for the reason given above.

**Verification**
The host case is the real evidence, because the condition cannot be booted. `//kernel/lib/acpi/test:numa` gained a case that builds a genuine 40-locality SLIT — 1600 entries, nothing lying about its length — and asserts that the library accepts it unclamped, that every entry of the leading 32×32 square reads back what was written at the full stride, and that the rows past it are still present. That is the premise the clamp rests on, pinned so that a later change to either half fails rather than silently producing wrong distances.

```text
host/obj/kernel/lib/acpi/numa-test     exit 0
```

On the four-node machine the clamp is inert, which is the other thing to show:

```text
diff (four-node fallback output, before the clamp vs after)
  (no output -- 24 lines identical)
```

```text
build      ok
dbg        PASS  44 passed, 4 skipped, 0 failed      (uniform and four-node alike)
rel        PASS  41 passed, 8 skipped, 0 failed · 6/6 required
host-tests PASS  151 passed, 0 failed
pmnode.test: ok -- 20 checks · pmzone.test: ok -- 3 checks
```

**Deliberately not done**
`Topology::set_distance()` still guards its array with a `DEBUG_ASSERT` only. With the clamp in place no in-tree caller can exceed it, so this is about the next caller rather than this one; folding it in would also have mixed a library interface change into an arch-side bug fix.

The repair item's own acceptance criterion said to put the host case in `//kernel/lib/numa/test:fallback_order`. That was wrong when it was written: the clamp belongs in arch code, which is not host-compilable, so what is testable on the host is the library premise rather than the clamp itself. The case went to `//kernel/lib/acpi/test:numa`, beside the other `DistanceMatrix` cases.

Item 2 — `register_meminfo()` reporting success having re-homed only some regions — stands, and is the other half of making a truncated topology coherent.

**How it was found**
Not by me. Three reviewers of the previous change each raised it independently, from different lenses, and each of their skeptics confirmed the mechanism line by line and then refused it as a defect of that diff: pre-existing, untouched, and unreachable on either machine the project boots. That verdict was right, and the finding was worth keeping, which is why it became a repair item rather than either a hurried fix or nothing.

Worth noting what made the skeptics' refusals useful rather than obstructive: each one did the arithmetic. One of them worked out that `DISTANCE` is 32×32 `usize` = 8192 bytes, that `from_table()`'s check admits a count up to roughly the square root of the payload, and that at level 0 the only guard is discarded — and then said, having established all of that, that it still was not this change's bug. "True, and not a defect of this diff" is a different verdict from "false", and a review that collapses the two either inflates the change under review or loses the finding entirely.

**The user's prediction**
None — homework is paused.

**Three questions you should be able to answer**
1. Three separate bounds checks exist on the way from a SLIT to `DISTANCE`, and the array was still writable past its end. Say what each one checks and why none of them is this one.
2. The truncated matrix's kept distances are correct, which is the whole argument for truncating rather than refusing. Explain why, in terms of how `DistanceMatrix::distance()` computes its index.
3. The host case does not test the clamp. Say what it does test, and why that is the most that can be tested on the host.

## The SRAT's memory affinities report how much of themselves were applied (2026-10-29)

**Files**
- `kernel/arch/x86/mm/srat.cpp` — `register_meminfo()` separates the two conditions that shared one guard, counts the three outcomes into a `MemoryAffinityReport`, and logs an error for a refused range; `acpi_init_numa()` logs the counts and warns when the topology was truncated.

**Symptom**
On a machine whose SRAT names more proximity domains than `MAX_NODES`, the regions belonging to the domains above the ceiling silently kept node 0 while their neighbours moved, and `acpi_init_numa()` was told `Status::Ok`. Nothing in the log mentioned them. Now observable on `NUMA=33`, where it reads:

```text
[info] numa: the ACPI SRAT describes 34 memory region(s), 33 re-homed
[warn] numa: 1 of them name a domain beyond the 32 nodes this kernel holds, so that memory stays on node 0
```

Before the change that machine said nothing at all between `NUMA information from ACPI-SRAT` and the SLIT line.

**Cause**
One `if` carried two unrelated conditions:

```cpp
if (assignment_status == Status::Ok && region.domain < MAX_NODES) {
```

The left half is an error latch -- once `EarlyMem::set_node()` has failed, stop trying -- and it is right. The right half is a capability test, and sharing the latch's `if` is what made it wrong: a region whose domain id this kernel cannot represent fell out of the body without touching `assignment_status`, so the function returned the initial `Status::Ok`. A boot that re-homed every region and a boot that re-homed none of them were indistinguishable, to the caller and in the log.

The comment that stood above it justified the skip by machines that are not NUMA. That covers a domain id of 0 on a uniform machine, which is the case it was written for. It does not cover a domain id beyond the node ceiling: that is a truncated topology, and the kernel should say so, because the memory of a domain it dropped stays on node 0 and will be allocated as though node 0 owned it.

Three outcomes per region, one `Status` to report them in: re-homed, left behind because the id does not fit, or refused by bootmem. A `Status` cannot say "some", which is why the fix is a count and not a different error code.

**Change**
The guards are separate, each with its own reason, and the three outcomes are counted:

```cpp
    struct MemoryAffinityReport {
        u32 described{};
        u32 rehomed{};
        u32 truncated{};
    };
```

```cpp
                if (failure != Status::Ok) {
                    return;
                }
                if (region.domain >= MAX_NODES) {
                    report->truncated += 1;
                    return;
                }
                failure = mm::EarlyMem::set_node(region.base, region.size, NodeId{region.domain});
                if (failure != Status::Ok) {
                    log::error("numa: [0x{:X}, 0x{:X}) would not move to node {} ({})", ...);
                    return;
                }
                report->rehomed += 1;
```

The caller states the counts whatever happened, and warns when any region was left behind.

A truncated topology still returns `Status::Ok`, which is a deliberate departure from the repair item's own acceptance criterion ("the status `acpi_init_numa()` sees must not claim success for a partial application") and from Linux. The reason is that an error here is strictly worse than the truth in the log, for two separate reasons. Nothing rolls back: by the time a region is found to be out of range, earlier regions have already been re-homed and their nodes marked `Possible`, and `find_possible_nodes()` reads those marks. And `x86_init_numa()` treats a failure from `acpi_init_numa()` as "no firmware topology", tries the PCI fallback, and logs that there was none -- which would be false on a machine that just brought up 32 nodes from the SRAT, and is exactly the misleading line fixed in `7814aa93`. So the status answers "is what I applied coherent", the counts answer "how much did I apply", and the warning is what a reader follows to find out why one node holds memory the SRAT gave to another.

Rejected: ending the walk on a refused range, as the repair item suggested. The library's visitor returns `void`, so the walk cannot be stopped from inside it; giving it a `bool` return is a change to `acpi::numa::for_each_memory_region()` and its other caller, which does not belong in a fix to this one. The remaining regions are counted and skipped instead, and the comment says why.

**Reference**
Linux's `acpi_numa_memory_affinity_init()` in `arch/x86/mm/srat.c` rejects the whole table through `bad_srat()` when an entry is unusable, and `numa_init()` then falls back to `dummy_numa_init()` -- one node over all memory. It refuses to create a partial application at all. That works there because the rejection happens before anything is committed: `numa_add_memblk()` appends to a `numa_meminfo` array that `numa_init()` discards wholesale on failure, so Linux has the rollback this kernel does not. `EarlyMem::set_node()` writes straight into the live bootmem region list.

**Verification**
- `NUMA=33` (34 memory regions over 33 domains, one above the ceiling), both images: dbg 44 passed 4 skipped, rel 41 passed 8 skipped 6/6 required. Both printed the two lines quoted above.
- `NUMA=1` (the designed four-node machine), both images: dbg 44 passed, rel 41 passed 6/6, and `numa: the ACPI SRAT describes 5 memory region(s), 5 re-homed` with no warning -- five regions for four nodes, because node 0's memory is split by the low-memory hole.
- `out/bringup.x86`: build ok, dbg 44 passed, rel 41 passed 6/6, host-tests 151 passed.
- `pmnode.test` and `pmzone.test` on bringup: ok, 20 checks and 3 checks. `pmnode.test` on `NUMA=1`: ok, 56 checks. On `NUMA=33`: ok, 392 checks.
- clang-format clean on the changed lines.

**Deliberately not done**
- `print_numa_info()` still walks the memory regions a second time to trace them. It is pre-existing and harmless, and merging it into the counting walk would mix a tidy-up into a fix.
- `Topology::set_distance()` keeps only its `DEBUG_ASSERT` bound. Same reason as the clamp entry above: no caller in the tree can now pass it an out-of-range id.
- The no-SLIT early return this machine exposed is left as a repair item, not fixed here. See `docs/repairs/261029/mm.md`.

**How it was found**
Raised by a reviewer of the silence fix, confirmed by a skeptic, and refused as a defect of that change. Making it observable came first: `scripts/qemu_boot.py` learned to generate a machine with any number of nodes (`959fe947`), and the first `NUMA=33` boot printed nothing about the 33rd domain while `pmm: 32 node(s) own memory` three lines later proved it had been dropped.

**The user's prediction**
None — homework is paused.

**Three questions you should be able to answer**
1. Two conditions shared one `if`, and only one of them was wrong there. Say which, and what the other one is for.
2. Why does a truncated topology return `Status::Ok` here, when Linux refuses the whole table? Name the thing Linux has that this kernel does not.
3. `register_meminfo()` sees 34 memory regions on a 33-node machine. Where does the extra one come from?

## A CPU on a proximity domain with no memory runs from a node that exists (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/node_placement.hpp`, `kernel/mm/node_placement.cpp` — `seed_from_firmware()` takes the set of nodes that exist and adopts a firmware node only when it is in it; it returns how many CPUs it refused.
- `kernel/mm/pm_node_init.cpp` — `bootstrap()` passes the possible mask and logs the refused CPUs; a comment that said the possible mask is always empty at that point now says when it is not.
- `kernel/mm/pm_node.cpp` — `claim_possible_cpus()` passes the possible mask.
- `kernel/commands/pm_node_test.cpp` — the "no CPU the firmware placed was moved" check excludes CPUs placed on a node that does not exist, and a new check requires those to run from one that does.

**Symptom**
On a machine whose SRAT gives a proximity domain CPUs and no memory, the debug image panicked during bring-up and the release image booted on through a null pointer. Booted on exactly HEAD with node 0 holding all 512M and CPUs 0-3, and node 1 holding CPUs 4-7 and no memory:

```text
[info] numa: the ACPI SRAT places 8 CPU(s)
[info] pmm: 1 node(s) own memory
OURS KERNEL PANIC
[cpu0 error] Assertion failed on node with message: CPU 4 was claimed by node 1, which does not exist
```

The release image of the same machine reached the end of its boot with no symptom at all. After the change, both images boot, and say what happened:

```text
[info] pmm: the firmware put 4 CPU(s) on a node with no memory; they run from node 0
[info] pmm: 1 node(s) own memory
```

**Cause**
Two facts that live in different places disagreed, and the code that joins them checked only one.

Which nodes exist is decided by memory. `find_possible_nodes()` marks a node `Possible` for each usable memory range it owns, `register_meminfo()` does the same for each SRAT memory region it re-homes, and `create_nodes()` makes a `PmNode` for exactly the possible mask. A domain that owns no memory is never marked, so `NODES[1]` stays null.

Which node a CPU is on is decided by the firmware's CPU affinity entries, which name proximity domains whether or not those own memory. A socket with no DIMMs is an ordinary machine, and so is one whose only memory is hot-pluggable and therefore disabled in the SRAT.

`seed_from_firmware()` is where the two meet, and it checked the firmware's answer against `MAX_NODES` only:

```cpp
        if (declared < MAX_NODES) {
            set_node_of(cpu, declared);
        } else {
            set_node_of(cpu, fallback);
        }
```

`MAX_NODES` is how many nodes this kernel *could* hold, not which ones it *did* bring up. Node 1 passed, CPUs 4-7 were placed on it, and `publish_cpu()` looked it up:

```cpp
    auto *const node = PmNode::node(nid);
    DEBUG_ASSERT(node, "CPU {} was claimed by node {}, which does not exist", cpu, nid);
```

On the debug image that is the panic. On the release image `DEBUG_ASSERT` is `if constexpr (KERNEL_DEBUG_LEVEL)` and generates nothing, so the next lines took `node->get_lock()`, set a bit in `node->m_work_cpus`, and cached the null pointer as those CPUs' local node -- all through address 0 plus a member offset -- and the boot carried on with no visible symptom. That is the worse of the two outcomes: the debug image at least names the problem.

It was latent because neither machine this repo boots can produce it: `scripts/qemu_boot.py` gives every node a memory backend. It became urgent because the next change, making a missing SLIT non-fatal, lets `bind_node_to_cpu()` run on machines without a SLIT, where before it never did -- so the set of machines reaching this widened from "SRAT, SLIT and a memory-less domain" to "SRAT and a memory-less domain". Commit order was chosen so that no commit in the history turns a machine that booted into one that panics.

**Change**
The seed is told which nodes exist, and treats a firmware node outside that set as no answer:

```cpp
auto NodePlacement::seed_from_firmware(NodeId const fallback, NodeMask const &nodes) -> usize {
    ...
        if (declared < MAX_NODES && nodes.test(declared)) {
            set_node_of(cpu, declared);
            return;
        }
        if (declared < MAX_NODES) {
            refused += 1;
        }
        set_node_of(cpu, fallback);
```

Both callers pass the possible mask, which at both call sites is exactly the set `create_nodes()` makes a node for. The firmware table is not touched: `firmware_node(4)` still answers 1, because that is what the firmware said, and it is what hard affinity is built from -- so CPUs 4-7 are in no node's hard affinity, which is true of them.

The mask is a parameter rather than read from `PmNode` inside the seed, so that `NodePlacement` states its dependency in its signature instead of reaching into the node table, which `numa-out-of-mm`'s step 5 is about to move.

Counting needed the walk restructured. The seed used to walk the possible mask and then seed the running CPU by name, which would have counted the boot CPU twice if it were itself on a memory-less domain; it now walks the union once.

`bootstrap()` reports the count, because it is the firmware's placement overruled and a reader of the boot log should not have to infer it from `pmnode.test`. The second seed, after the MADT, does not repeat it: the MADT adds only CPUs the SRAT did not bind.

`pmnode.test`'s placement check asserted that no CPU the firmware placed had been moved. That is now deliberately false for a CPU placed on a node that does not exist, so the check excludes those, counts them in the log, and a new verdict requires each of them to run from a node that does exist -- the invariant this change establishes.

Rejected for now: bringing up a domain with CPUs and no memory as a real node with empty zones, and having its CPUs allocate from the nearest node with memory. That is the more complete model and changes mm's idea of what a node is; the user deferred it as its own design question.

**Reference**
Older x86 Linux did what this does: `init_cpu_to_node()` in `arch/x86/mm/numa.c` moved a CPU whose node was not online to another node, via `find_near_online_node()`, which picks the nearest by `node_distance()`. Later Linux instead brings such a node up with no memory -- for a time through `init_memory_less_node()` -- and keeps its CPUs on it; an allocation from one of them finds the node's own zones empty and falls through its zonelist, which `build_zonelists()` orders by distance, to the nearest node that has memory. This change takes the first shape, with one difference: it sends the CPUs to the fallback node -- the lowest-numbered node that owns memory -- rather than the nearest one.

**Verification**
- Memory-less node 1 with a SLIT, on exactly HEAD (the change set aside, then restored): dbg panics with `CPU 4 was claimed by node 1, which does not exist`.
- The same machine with the change: dbg boots and `pmnode.test` passes, `ok -- 21 checks`, logging `8 of 8 possible CPU(s) were placed by firmware` and `4 of them on a node with no memory`; rel reaches the end of its boot and logs the refusal.
- `out/bringup.x86`: build ok, dbg 44 passed, rel 41 passed 6/6, host-tests 151 passed; `pmnode.test` ok, 21 checks (20 before), `pmzone.test` ok, 3 checks.
- `NUMA=1`, both images: dbg 44, rel 41 6/6; `pmnode.test` ok, 57 checks (56 before); the fallback dump is identical line for line to the run before the change.
- `NUMA=33`, both images: dbg 44, rel 41 6/6; `pmnode.test` ok, 393 checks (392 before).
- clang-format clean on the changed lines; clang-tidy against `out/numa.x86` reports nothing on a changed line (four pre-existing diagnostics in these files, on lines this change does not touch).

The memory-less machine is not one `scripts/qemu_boot.py` can describe. It is booted by passing QEMU the arguments directly, which `qemu_boot.py` forwards -- with `NUMA=0 SMP=8 MEM=512`, and never in the foreground:

```text
-object memory-backend-ram,size=512M,id=m0
-numa node,nodeid=0,cpus=0-3,memdev=m0
-numa node,nodeid=1,cpus=4-7
-numa dist,src=0,dst=1,val=20 -numa dist,src=1,dst=0,val=20
```

**Deliberately not done**
- Memory-less nodes as real nodes, and the nearest node rather than the fallback: see Change.
- `publish_cpu()` keeps a `DEBUG_ASSERT` rather than a real check. The invariant it guards is now established where the table is written, and a release-image check there would only turn a silent null into a silent skip.
- `PmNode::set_cpu_node()`, the run-time move, does not check that its target exists. Nothing in the tree calls it with a node it did not get from the node table.
- No `qemu_boot.py` option for a memory-less node. Whether that machine belongs in the harness is a question for the user, alongside `NUMA=1` and `NUMA=33` in the standard check.

**How it was found**
By the review of the next change, which makes a missing SLIT non-fatal. Two of its five lenses -- the control flow, and the state the rest of the kernel newly sees -- each walked from `bind_node_to_cpu()` to the null in `publish_cpu()`, and both skeptics confirmed the chain and ruled it pre-existing. QEMU accepts a NUMA node with CPUs and no `memdev`, so the machine was built and booted with and without a SLIT, then on exactly HEAD: the debug image panicked each time, and the release image booted with no symptom.

**The user's prediction**
None -- homework is paused.

**Three questions you should be able to answer**
1. `MAX_NODES` and the possible mask both answer "is this a node?". Say what each one actually means, and which one `seed_from_firmware()` should have been asking.
2. Why did the release image boot on cleanly where the debug image panicked, and why is that the worse outcome?
3. CPU 4 now runs from node 0 while `firmware_node(4)` still answers 1. Why is it right to leave the firmware table saying 1, and what does that mean for node 0's hard affinity?

## A machine with an SRAT and no SLIT has a topology, so ACPI discovery stops failing for one (2026-10-29)

**Files**
- `kernel/arch/x86/mm/srat.cpp` — `register_nodedis()` returns `void`; `acpi_init_numa()` runs the distance and CPU stages to the end and returns `Ok` once the memory affinities are applied.
- `kernel/arch/x86/mm/numa.cpp` — the comment on `x86_init_numa()`'s fallback line loses the example this change makes unreachable.

**Symptom**
On a machine whose firmware declares proximity domains but no distances between them, no CPU was ever bound to a node, every node's hard affinity came out empty, and the boot log stated the opposite of what the kernel had just done. On `NUMA=33`, before:

```text
[info] numa: no ACPI SLIT; every node reads as equidistant
[info] numa: no firmware topology (acpi NotFound, pci NotSupported)
[info] pmm: 32 node(s) own memory
```

`numa: the ACPI SRAT places N CPU(s)` never appeared, and `pmnode.test` reported `0 of 8 possible CPU(s) were placed by firmware`. After:

```text
[info] numa: no ACPI SLIT; every node reads as equidistant
[info] numa: the ACPI SRAT places 8 CPU(s)
[info] pmm: 32 node(s) own memory
```

and `8 of 8 possible CPU(s) were placed by firmware`.

**Cause**
`register_nodedis()` returned `Status::NotFound` when the machine has no SLIT -- true about the table, and read by its caller as a verdict on the whole topology:

```cpp
    status = register_nodedis(*tables);
    if (status != Status::Ok) {
        // Already said which way it failed, and said it with the reason.
        return status;
    }
```

Two consequences follow from that one early return, and the second is worse than the first.

`bind_node_to_cpu()` never ran. `Topology::set_firmware_node()` was therefore never called, every entry of the firmware placement table kept its sentinel, `PmNode::init()` built each node's hard affinity from that table and so built them all empty, and `NodePlacement::seed_from_firmware()` put every CPU on its fallback -- the lowest-numbered node that owns memory, node 0 on `NUMA=33`. A 32-node machine came up with all eight CPUs on node 0, so `Gaf::ThisNode` meant node 0 from every CPU, and the hard affinity that is supposed to say which CPUs belong to a node said nothing at all.

And `x86_init_numa()`, seeing a non-Ok status, tried the PCI fallback and then logged `no firmware topology`. That line is false on a machine that has just registered 32 nodes from the SRAT, and nothing undoes them on the way out: `register_meminfo()` has already re-homed the memory and marked each domain `Possible`, and `find_possible_nodes()` reads those marks. The line exists precisely to stop a NUMA boot being mistaken for a healthy uniform one (`7814aa93`), and here it created the inverse mistake.

Why a missing SLIT is not a failure to discover the topology: the SRAT declares which proximity domains exist and what memory each owns, which is the topology. The SLIT declares how far apart they are, which refines it. A machine with no SLIT has nodes whose distances are unknown, and unknown distances degenerate to node-id order -- which `register_nodedis()` already handled, by zeroing the matrix, in the same breath as it returned the error that threw the rest away.

Why it stayed hidden until today: the only NUMA machine this repo could boot is built with explicit `-numa dist` arguments, so its firmware always carries a SLIT, and that path returns `Ok`. The machine that exposes this was built a few hours earlier, for the previous fix (`959fe947`), and it declares no distances.

**Change**
`register_nodedis()` returns nothing:

```cpp
    auto register_nodedis(acpi::TableSet &tables) -> void {
```

Each of its three ways out already logged which one it was -- no SLIT, a rejected SLIT, a SLIT with its locality count -- and all three leave the matrix in a state the fallback order reads. There is nothing left for a caller to decide, so there is nothing to hand it. A rejected SLIT is now non-fatal for the same reason as an absent one, which is the question the repair item asked to be decided explicitly rather than by reusing one status for both.

The caller runs both remaining stages to the end:

```cpp
    register_nodedis(*tables);

    if (auto const binding = bind_node_to_cpu(*srat); binding != Status::Ok) {
        log::warn("numa: the SRAT's CPU affinity list ended early ({}); the CPUs it did name"
                  " are placed",
                  to_string(binding));
    }

    return Status::Ok;
```

So `acpi_init_numa()` now reports one thing: whether this machine's ACPI tables gave it a topology. It returns non-Ok when there are no tables, no SRAT, or the SRAT's memory-affinity walk failed, and `Ok` once the memory affinities are applied. The first two hand the PMM nothing. The third is not that clean: a walk that fails part-way has already re-homed the regions before the failure and marked their nodes `Possible`, so the PMM is handed part of a topology while the log says there was none -- the same shape this entry is about, one stage earlier. It is left for its own change (`docs/repairs/261029/mm.md`), because what a half-applied memory map should become is a different question from whether distances are optional.

The `bind_node_to_cpu()` early return was not in the repair item; it is the same defect in the same function, one stage later. A malformed CPU-affinity list leaves the CPUs the walk reached placed, and the ones it never reached unknown to the topology altogether: not in the possible mask, so `PmNode::bootstrap()` does not see them, and with no firmware node, so no node's hard affinity ever holds them. The MADT reports them later and `PmNode::claim_possible_cpus()` puts them on the boot CPU's node -- somewhere to run, not necessarily where they are. That is a degraded topology, and still better than discarding one whose memory affinities cannot be rolled back, so it warns and carries on. (The first draft of this paragraph, and of the comment beside the code, said `bootstrap()` fills the missing CPUs "from where each CPU is actually running". Nothing does that; four reviewers found it independently.) Fixing one and leaving the other would have left the same false log line reachable from the next stage down.

Letting `bind_node_to_cpu()` run on machines it never ran on before has a consequence the first draft missed. On a machine whose SRAT gives a proximity domain CPUs and no memory, the CPUs it binds there were placed on a node that is never created, and the debug image panicked in `publish_cpu()`. That was already true of such a machine with a SLIT; without one, this change would have newly exposed it, turning a machine that booted -- badly -- into one that panics. The review found it, and it was fixed first, as its own change (`f85f9670`), so that no commit in the history makes that trade.

Rejected: keeping the `Status` and filtering it at the call site (`if (status != Ok && status != NotFound)`). It works, and it leaves the trap in place -- the next reader of that status has to know that one of its values means "fine" -- which is the mistake this entry is about. A function whose outcome no caller may act on should not hand one out.

Also rewritten: the paragraph in `numa.cpp` explaining why the fallback line does not claim a node count. The argument is unchanged and still holds, but its example was a machine with an SRAT and no SLIT, which no longer reaches that line. The example is now a memory-affinity walk that stops part-way, which does.

**Reference**
Linux's `acpi_numa_init()` in `drivers/acpi/numa/srat.c` calls `acpi_table_parse(ACPI_SIG_SLIT, acpi_parse_slit)` and does not look at the result; `acpi_parse_slit()` itself checks the table with `slit_valid()` and, when it is not, prints that the SLIT looks invalid and is not used, leaving `node_distance()` on `LOCAL_DISTANCE`/`REMOTE_DISTANCE` and the SRAT's topology standing. Only an SRAT failure reaches `bad_srat()` and collapses the machine to `dummy_numa_init()`. The shape copied here is that division: one table decides whether there is a topology, the other only refines it.

**Verification**
On top of `f85f9670`:

- `NUMA=33`, both images: dbg 44 passed 4 skipped, rel 41 passed 8 skipped 6/6 required. Both print `numa: the ACPI SRAT places 8 CPU(s)`, and neither prints `no firmware topology` (counted: 0 in each log).
- `NUMA=33 scripts/kshell_run.py pmnode.test`: `ok -- 393 checks`, with `8 of 8 possible CPU(s) were placed by firmware` (`0 of 8` before).
- A memory-less node 1 with no SLIT -- the machine this change would have broken without `f85f9670` -- boots on both images: `numa: no ACPI SLIT`, `numa: the ACPI SRAT places 8 CPU(s)`, `pmm: the firmware put 4 CPU(s) on a node with no memory; they run from node 0`, and `pmnode.test` on dbg `ok -- 21 checks`. Booted by passing QEMU the arguments recorded in the `f85f9670` entry, minus the two `-numa dist`.
- `NUMA=1`, both images: dbg 44 passed, rel 41 passed 6/6, and the whole fallback dump is identical line for line to the run before this change -- node 1 -> 1,0,3,2, node 2 -> 2,3,0,1, node 3 -> 3,2,1,0. That machine has a SLIT, so nothing in its path moved; the diff proves it. `pmnode.test` there: `ok -- 57 checks`.
- `out/bringup.x86`: build ok, dbg 44 passed, rel 41 passed 6/6, host-tests 151 passed; `pmnode.test` ok, 21 checks, `pmzone.test` ok, 3 checks.
- clang-format clean on the changed lines; clang-tidy against `out/numa.x86` reports nothing anywhere in the three files.

**Deliberately not done**
- `bind_node_to_cpu()` keeps its `Status`, because the caller does act on it -- it decides whether to warn. Only the early return went.
- The `FIXME` in `bind_node_to_cpu()` stands: it still sets `cpu_possible_mask()` from the SRAT walk, which is the MADT's job, so a malformed walk leaves CPUs out of the possible mask until the MADT parse adds them. That is a pre-existing question of which table owns that fact, not of this control flow. Nor does its `count >= MAX_CPU` cut-off say anything when it drops a CPU; the count it logs is the only trace.
- A memory-affinity walk that fails part-way keeps its early return, and with it the shape this entry removes: nodes registered, CPUs unbound, and a log saying there was no topology. Recorded in `docs/repairs/261029/mm.md`.
- A SLIT that declares fewer localities than the SRAT names domains leaves the undescribed nodes at distance 0, nearer than any real one, so they lead every fallback chain. Pre-existing, firmware-bug-only, and recorded alongside.
- Nodes that own memory and have no CPU are now the common case on a generated machine (24 of 32 on `NUMA=33`) and nothing was changed for them. `pmnode.test` passes there, which is evidence and not a proof.
- `pci_init_numa()` is untouched. It is now tried only when ACPI discovery failed outright or its memory walk failed part-way; and no strong definition of it exists anywhere in the tree, so it only ever answers `Unsupported`.

**How it was found**
By the machine built to accept the previous fix. The first `NUMA=33` boot's log carried `no firmware topology` three lines above `pmm: 32 node(s) own memory`, which cannot both be true, and `pmnode.test` on the same machine reported that no CPU had been placed by firmware -- against 8 of 8 on the four-node machine. It had also been predicted: a reviewer of `7814aa93` raised the SRAT-without-SLIT case from the code alone, and it was recorded as a repair item rather than fixed because no machine could then reach it.

The change was then reviewed through five lenses, each finding handed to a skeptic told to refute it: 18 findings, 12 survived. Four lenses independently caught the same false sentence of mine -- that `bootstrap()` fills the CPUs a malformed walk missed "from where each CPU is actually running"; nothing does -- and three caught a twin comment in `pm_node_init.cpp` still describing the early return this change deleted. Both are corrected above. The one that changed the plan was the memory-less domain, which two lenses walked to the null in `publish_cpu()` independently: the user chose to fix it first.

**The user's prediction**
None -- homework is paused.

**Three questions you should be able to answer**
1. The old code returned a status that was *true* -- there really was no SLIT. Say exactly where the mistake was, and what the general shape of it is.
2. Two things broke from that one early return. One is a log line; the other is why a 32-node machine had nothing to prefer when allocating. Explain the second.
3. Why is a *malformed* SLIT also non-fatal now, and what would have to be true of this kernel for refusing it to be the better choice?

## Title: the PMM gets a gate for allocations that can wait, and a free-memory signal for whoever watches memory (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/gaf.hpp` — `Gaf::CanWait`
- `kernel/mm/include/ours/mm/pmm.hpp` — `try_alloc_frames()`, `wait_for_frame_allocation()`, `set_free_memory_signal()`, `refuse_delayed_allocations()`, `approx_free_frame_count()`
- `kernel/mm/pmm.cpp` — the gate, the signal, and a check of the signal after every allocation and free; the entries that answer a bare pointer or a Status assert they are not given CanWait
- `kernel/commands/memory_pressure_test.cpp`, `kernel/commands/BUILD.gn` — `mempressure.test`, new
- `docs/repairs/261029/mm.md` — new: the cost of the signal's check on every allocation and free

**Symptom**
None: a new feature, step 1 of `.agents/tasks/memory-pressure/PLAN.md`. Before it, a frame allocation that found no frame failed on the spot, whoever asked; nothing could tell a caller to wait, and nothing told anyone that memory was getting short.

**Cause**
Why each part has the shape it has, since that is the part worth learning:

- **A gate that stays shut.** A CanWait allocation that finds free memory below the delay level shuts the gate, and every later CanWait allocation answers ShouldWait, however many frames come back, until the watcher calls `set_free_memory_signal()` again. If the gate reopened by itself as soon as free memory was back above the level, the waiters would wake, take the few frames a free gave back, push free memory under the level again and shut it again: the gate would flap at the rate of frees, and the watcher, the one thing that can make memory (reclaim) or decide that none will come (D4), might never get to act. Sticky means the gate opens only after the watcher has seen the shortage.
- **The level not above the lower bound.** The watcher's event fires when free memory falls below its lower bound. With the delay level at or below that bound, free memory has crossed the bound before the gate can shut, so a shut gate means the watcher has been told. With the level above the bound the gate could shut while the watcher sleeps, and the waiters would wait for someone who is not coming. `set_free_memory_signal()` asserts it.
- **Waiters wake one at a time.** `MAY_ALLOCATE` is an auto-unsignal event: each signal wakes one waiter, which takes its frame and signals again for the next. A manual-reset event would wake every waiter on an opening, and all of them would race for the same frames, most to find the gate shut again. A waiter that finds the gate shut again does not pass the signal on: the next opening does. A refusal is passed on, so every waiter hears it.
- **CanWait is not DirectReclaim.** DirectReclaim says the caller may block inside the call; CanWait says the call returns at once with ShouldWait and the caller waits later, where it holds no lock (D2). Only `try_alloc_frames()` takes it, because only a Result can say ShouldWait: an entry answering a null pointer would have to serve the allocation past the gate or report OutOfMem, and either is wrong without the caller knowing. Those entries assert it.
- **Above PmNode.** Every allocation and free of a frame goes through `pmm.cpp`, so the gate and the signal see all of them there without touching `PmNode`, whose allocation path the unmerged `feat/pmm-cpp-vocabulary` rewrites, and the node stays without policy about who waits (D3). The price is that there is no running free count at this layer, only the zones' own counters, summed on each check: `docs/repairs/261029/mm.md` item 1.
- **Atomics on the hot side.** What every allocation and free reads (the gate's state, the level, the bounds, the event pointer) is atomics, each loaded on its own, so the allocation path takes no new lock. The bounds and the event are two loads, not one snapshot, so a CPU may compare with bounds newer than the event it loaded; that costs at most one extra signal, which only makes the watcher look again. The event that wakes waiters and the lock ordering the watcher's calls are touched only by waiters and by the watcher.
- **Signals outside the lock.** `set_free_memory_signal()` signals `MAY_ALLOCATE` after letting go of `WATCHER_LOCK`: a signal wakes a thread and takes the scheduler's lock, which should not nest inside a PMM spinlock.

**Change**
The gate's decision, in `admit_waitable()`:

```cpp
auto state = GATE.load(std::memory_order_acquire);
while (true) {
    if (state == GateState::Shut) {
        return Status::ShouldWait;
    }
    auto const level = DELAY_LEVEL.load(std::memory_order_relaxed);
    if (level == 0 || approx_free_frame_count() >= level) {
        return Status::Ok;
    }
    if (state == GateState::Refusing) {
        return Status::OutOfMem;
    }
    if (GATE.compare_exchange_weak(state, GateState::Shut, std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
        return Status::ShouldWait;
    }
}
```

The signal, after every allocation and free, one load when nothing is armed:

```cpp
if (SIGNAL_EVENT.load(std::memory_order_acquire) == nullptr) {
    return;
}
auto const free = approx_free_frame_count();
if (free >= SIGNAL_LOWER.load(std::memory_order_relaxed) &&
    free <= SIGNAL_UPPER.load(std::memory_order_relaxed)) {
    return;
}
if (auto *const event = SIGNAL_EVENT.exchange(nullptr, std::memory_order_acq_rel)) {
    event->signal();
}
```

`approx_free_frame_count()` is public because the levels must be set from the count they are compared with: it includes the frames in the per-CPU caches, which `free_frame_count()` does not, so bounds set from the exact count can be off by as much as those caches hold.

Rejected: the PMM as a PageProvider (the user's first shape, DESIGN.md); the gate inside `PmNode` (D3 (a)); a waiting `alloc_frames()` that sleeps inside the call (Linux direct reclaim), for the reason `6794ed44` removed reclaim from the allocation path.

**Reference**
Zircon: the delayed allocations of `PmmNode` (`ShouldWaitState`, `should_wait_free_pages_level_` and `may_allocate_evt_` in `zircon/kernel/vm/include/vm/pmm_node.h:359-379`), `pmm_set_free_memory_signal()` and its rule that the delay level is not above the lower bound (`zircon/kernel/vm/include/vm/pmm.h`), `PmmNode::WaitForSinglePageAllocation()` (`zircon/kernel/vm/pmm_node.cc:339`), and the free count checked on every change in `increment_free_count_locked()` and `decrement_free_count_locked()` (`zircon/kernel/vm/pmm_node.rs`). `PMM_ALLOC_FLAG_CAN_WAIT` is `Gaf::CanWait`. Differences: Zircon keeps the gate inside its one PMM node under its lock; here it is one layer up, on atomics.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- `mempressure.test`: ok, 6 checks. `largepage.test` and `pagesource.test` still ok.
- Controls: `admit_waitable()` always answering Ok fails 3 checks (the ShouldWait, the waiter, the refusal); `set_free_memory_signal()` not signalling `MAY_ALLOCATE` fails the waiter check (it gives up after 5 seconds). Each restored, the test passes.
- clang-tidy: nothing in the files changed; clang-format clean.
- Not measured: the cost of the signal's check (repair item above).

**Deliberately not done**
- Nothing passes CanWait yet: the VMO side is step 2, the watcher step 3, and until then the gate is open with no level and no event, so every allocation behaves as before.
- The bounds are checked both ways on every call. Zircon checks the lower bound only when the count goes down and the upper only when it goes up; with the zones' counters summed either way, splitting the check saves a compare, not the sum.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why does the gate stay shut when free memory comes back above the delay level, and what would go wrong if it reopened by itself?
- Why must the delay level not be above the watcher's lower bound?
- A waiter wakes, tries, and finds the gate shut again. Why does it not signal `MAY_ALLOCATE` for the next waiter, while one that was refused does?

## Title: a user VMO's page waits at the gate through an anonymous page request (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/page_request.hpp`, `kernel/mm/page_request.cpp` — `AnonymousPageRequest`, until now an empty placeholder, and `MultiPageRequest::wait()` over both of its requests
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `alloc_pages()` takes the request's frame or raises the request; `cow_page()` takes a request; a 2 MiB block goes through the gate without waiting
- `kernel/mm/include/ours/mm/gaf.hpp` — the preset `GAF_USER_VMO`
- `kernel/object/vmo_dispatcher.cpp`, `kernel/object/pager_dispatcher.cpp` — the VMOs a process makes, and a pager's, are made with it
- `kernel/commands/memory_pressure_test.cpp` — a store, a write and a commit of a user VMO, each with the gate shut

**Symptom**
None: step 2 of `.agents/tasks/memory-pressure/PLAN.md`. Before it, the gate of step 1 had no caller: nothing passed `Gaf::CanWait`.

**Cause**
Why each part has the shape it has:

- **The wait keeps the frame.** `AnonymousPageRequest::allocate()` does not just wait for the gate to open and let the retried fault ask the PMM again: it takes a frame itself (`wait_for_frame_allocation()`) and keeps it, and `alloc_pages()` on the retry takes that frame first. Asking again could meet a gate shut again by someone else, and a waiter that loses that race every time never gets a page. Holding the frame across the retry makes each wait progress.
- **The request survives the retry.** The frame lives in the request, so the request must outlive the pass that raised it: the three loops that wait (the fault in `VmAspace::soft_fault()`, `commit_range()`, `copy_page_by_page()`) already keep their `MultiPageRequest` outside the loop, for the page source's sake. A retry that finds the page there already, put there by another thread meanwhile, does not take the frame; the request's destructor gives it back.
- **Only one of the two is raised.** A call that answers ShouldWait has raised exactly the request it needed and returned at once, so `MultiPageRequest::wait()` waits on the anonymous one when it is active and on the read request otherwise.
- **No request means refused, not served.** Two callers allocate while holding locks they cannot let go of: `take_pages_locked()`, through `cow_page()`, holds the node's lock across a whole range. They pass no request, and a ShouldWait becomes OutOfMem, as for a refused waiter. Serving them past the gate instead would hand them the frames below the delay level, the ones the gate keeps for the kernel. Zircon asserts instead that a VMO allowed to wait is always given a request (`AllocateCopyPage()`, `zircon/kernel/vm/vm_cow_pages.cc:824`).
- **A block goes through the gate.** `fill_block_locked()` allocates 512 frames at once. Past the gate it would take them from the kernel's reserve while single pages wait; so it asks with the node's flags, and when turned away returns nothing, and the hole gets one page, which waits.
- **A preset, not GAF_USER.** `GAF_USER` also allocates the kernel objects a process causes (`new (mm::GAF_USER)` for every dispatcher), which come from the heap, whose entries cannot say ShouldWait. `GAF_USER_VMO` is for VMO pages only. A clone inherits its parent's flags, so a snapshot of a user VMO waits too; the VMOs a process gets pinned or wired (`create_paged()`, `create_from_wired_pages()`) do not.
- **A mapping that commits never meets it.** `VmMapping::map()` with `MapOptions::Commit` holds the address space's lock and cannot wait; its only caller is a kernel stack, never made with `CanWait`, and a user populate maps without commit (`vm_area.cpp`). The same assumption Zircon makes, with an assertion, at `zircon/kernel/vm/vm_mapping.cc:1032`.

**Change**
`alloc_pages()`, before:

```cpp
auto frame = alloc_frames(m_gaf | Gaf::Zero, order);
if (!frame) {
    return Status::OutOfMem;
}
```

After:

```cpp
auto const gaf = m_gaf | Gaf::Zero;
PmFrame *frame = nullptr;
if (request && request->has_frame()) {
    frame = request->take_frame();
} else {
    auto result = try_alloc_frames(gaf, order);
    if (!result) {
        auto const status = result.unwrap_err();
        if (Status::ShouldWait != status) {
            return status;
        }
        if (!request) {
            return Status::OutOfMem;
        }
        request->make_active(gaf);
        return Status::ShouldWait;
    }
    frame = *result;
}
```

`MultiPageRequest::wait()`, before `return m_read.wait(suspendable);`, after:

```cpp
if (m_anonymous.is_active()) {
    return m_anonymous.allocate(suspendable);
}
return m_read.wait(suspendable);
```

**Reference**
Zircon `AnonymousPageRequest::Allocate()` (`zircon/kernel/vm/anonymous_page_request.cc`), which waits in `PmmNode::WaitForSinglePageAllocation()` and keeps the page for `take_page()`; `VmCowPages::AllocateCopyPage()` taking `request->take_page()` first (`zircon/kernel/vm/vm_cow_pages.cc:821`); user VMOs made with `ALLOC_FLAG_CAN_WAIT` (`zircon/kernel/lib/syscalls/vmo.rs:54`). Differences: Zircon panics after 30 seconds of waiting, here the wait reports every 5 seconds and goes on, since the watcher of step 3 refuses the waiters when nothing will come back; and a caller without a request is refused rather than asserted on.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- `mempressure.test`: ok, 10 checks. `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`, `vmaundo.test`, `usercopy.test` ok.
- Controls: `alloc_pages()` never raising the request (every ShouldWait refused) fails the 3 VMO checks; user VMOs allocating past the gate (`CanWait` masked off) fails the same 3, since nothing sleeps. Each restored, the test passes.
- clang-tidy: nothing on the lines changed (one finding, `modernize-use-override` on `~VmCowPages()`, is on a line this did not touch); clang-format clean.

**Deliberately not done**
- A wait that never ends is reported, not ended: it ends when the gate opens or the watcher refuses (step 3). Until step 3 nothing shuts the gate, so no wait happens outside the test.
- A wait for content from a page source and a wait for a frame are never needed by the same call, so `MultiPageRequest` does not try to raise both.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why does the wait take a frame and keep it, instead of only waiting for the gate to open and letting the retried fault allocate?
- Why is a caller with no request refused, rather than served past the gate?
- Why can `GAF_USER` itself not carry `CanWait`?

## Title: a memory watchdog thread turns free memory into a pressure, and refuses the allocations that could wait once memory is out (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/memory_watchdog.hpp`, `kernel/mm/memory_watchdog.cpp`, `kernel/mm/BUILD.gn` — new: `MemoryWatchdog`, its thread, started at `InitLevel::Threading + 1`
- `kernel/include/ours/boot/options.hpp`, `kernel/top/boot_options.cpp` — `kernel.oom.outofmemory-mb`, `critical-mb`, `warning-mb`, `debounce-mb`, `hysteresis-seconds`
- `kernel/mm/include/ours/mm/pmm.hpp`, `kernel/mm/pmm.cpp` — `set_free_memory_signal()` takes what a delayed allocation gets (`DelayedAllocations::Wait` or `Refuse`); `refuse_delayed_allocations()` is gone into it
- `kernel/commands/memory_pressure_test.cpp` — the watchdog's checks; the checks of the gate run under `MemoryWatchdog::Hold`
- `docs/repairs/261029/mm.md` — item 1's cost is now paid from boot

**Symptom**
None: step 3 of `.agents/tasks/memory-pressure/PLAN.md`. Before it, nothing set the gate's level: steps 1 and 2 worked only when a test set it by hand.

**Cause**
Why each part has the shape it has:

- **Four pressures.** Normal, Warning, Critical, OutOfMemory, each beginning below a level of free memory. Zircon has a fifth, ImminentOutOfMemory, which tells user space to take diagnostics before it is too late; nothing here would read it, so it was left out (the user, 2026-10-29).
- **Debounce.** The bounds the watchdog arms are its pressure's range widened by the debounce on both sides. Free memory that moves to and fro across a level by a few frames, as every allocation and free makes it, then never wakes the watchdog; it wakes only once free memory has really gone past.
- **Hysteresis, for the declared pressure only.** A higher pressure is declared at once, a lower one only once the hysteresis has passed since the last change, so that a load that comes and goes is not logged as a change each time. What the PMM is armed with follows the free memory actually found: the gate opens as soon as memory is back, and only the log waits.
- **The delay level is the lower bound at Critical.** The level sits the debounce below the OutOfMemory level, which is exactly where Critical's bounds end. So the one allocation that takes free memory past it both shuts the gate and signals the watchdog, which then declares OutOfMemory and arms the PMM with Refuse: the waiters wake and are refused (D4 (a)), and the frames below the level are left to the kernel's own allocations. Nothing waits for a reclaim that cannot come.
- **Refuse as an argument, not a second call.** At OutOfMemory the watchdog arms the signal (to hear when memory is back) and refuses in one step. As two calls, `set_free_memory_signal()` would first have opened the gate in Wait with a level above its lower bound of 0, which its own assertion forbids, since a shut gate could then wait for a watcher who is not coming. With the mode an argument, the assertion stays and says exactly that: with Wait the level is not above the lower bound; with Refuse nobody waits.
- **A Hold for tests.** The gate's own checks set it by hand, which disarms the watchdog's event. `MemoryWatchdog::Hold` takes the lock the watchdog arms under, so it cannot arm meanwhile, and when the Hold goes it wakes the watchdog to arm again. `set_levels()` moves the levels to where free memory is, which reaches every pressure with a few hundred frames instead of eating 440 MiB (the user, 2026-10-29).

**Change**
The watchdog's turn, shortened:

```cpp
auto const found = pressure_for(levels, approx_free_frame_count());
auto declare = first || SKIP_HYSTERESIS || found > declared;
if (!declare && found < declared) {
    if (now - declared_at >= levels.hysteresis) {
        declare = true;
    } else {
        deadline = time::Deadline::at(declared_at + levels.hysteresis);
    }
}
// ... log and publish |found| if declared ...
auto delayed = DelayedAllocations::Wait;
if (found == MemoryPressure::OutOfMemory) {
    delayed = DelayedAllocations::Refuse;
}
auto const bounds = bounds_for(levels, found);
if (!set_free_memory_signal(bounds.lower, bounds.upper,
                            levels.out_of_memory - levels.debounce, &SIGNAL, delayed)) {
    continue;
}
// ... then, the lock let go:
auto _ = SIGNAL.wait(deadline);
```

The bounds, for levels `oom < critical < warning` and debounce `d`: Normal `[warning - d, ∞)`, Warning `[critical - d, warning + d]`, Critical `[oom - d, critical + d]`, OutOfMemory `[0, oom + d]`.

**Reference**
Zircon `MemoryWatchdog` (`zircon/kernel/object/memory_watchdog.cc`): `WorkerThread()`, `WaitForMemChange()` with `FreeMemBoundsForLevel()` and the delay level `mem_watermarks_[kOutOfMemory] - watermark_debounce_`, the hysteresis in `IsSignalDue()`, and `OnOom()`; the boot options `kernel.oom.*` and their defaults (`zircon/kernel/lib/boot-options/include/lib/boot-options/options.inc`, 50, 150, 300 and 1 MiB, 10 seconds). Differences: no ImminentOutOfMemory, no eviction, no events for user space; at OutOfMemory the waiters are refused where Zircon reboots or kills a job (`OomBehavior`); and the hysteresis holds back only the declared pressure, where Zircon's also holds back re-arming.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed. Both images log `mm: memory pressure Normal, 491 MiB free` at boot.
- `mempressure.test`: ok, 15 checks, the transitions in the log: Warning, Critical, Warning, OutOfMemory with its warning, Normal. `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`, `vmaundo.test`, `usercopy.test` ok.
- Controls: arming Wait at OutOfMemory panics on `set_free_memory_signal()`'s assertion ("a delay level 125800 above the lower bound 0"), the invariant the mode argument keeps; arming Refuse with a level of 0, which refuses nothing, fails the OutOfMemory check; arming no event, so that only `set_levels()` wakes the watchdog, fails the Critical check. Each restored, the test passes.
- clang-tidy: nothing in the files changed; clang-format clean.
- Not measured: the cost of the signal's check, now paid by every allocation and free from boot (`docs/repairs/261029/mm.md` item 1).

**Deliberately not done**
- Eviction, compression and compaction: nothing to reclaim yet. The watchdog is where they would be woken.
- Pressure events for user space, and killing a process at OutOfMemory (D4 (b)): later, with a component that acts on them.
- The thread runs at the default priority. Under pressure it has one thing to do per crossing, and the waiters it refuses are the ones it would compete with.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why is the delay level put exactly the debounce below the OutOfMemory level, and what happens in the one allocation that crosses it?
- What would go wrong without the debounce, with free memory sitting right at a level?
- Why does the hysteresis hold back the declared pressure but not the gate?

## Title: the memory watchdog is an object that owns its state, not a class of statics over file-scope globals (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/memory_watchdog.hpp`, `kernel/mm/memory_watchdog.cpp` — `MemoryWatchdog` holds its event, lock, levels and declared pressure as members, and its thread is `run()`; the questions about the levels are methods of `MemoryPressureLevels`
- `kernel/commands/memory_pressure_test.cpp` — reaches the watchdog through `MemoryWatchdog::get()`

**Symptom**
No change in behaviour. The user asked, 2026-10-29, why the watchdog was a class of static functions over a lock, an event, the levels and the declared pressure kept as globals in an anonymous namespace, with its thread and its two pure helpers as free functions.

**Cause**
It broke the rule of `modern-cpp-development` ("Model with objects ... Reach for a free function only when there is no state to guard"): the watchdog has state and an invariant over it (the levels and the declared pressure change only under the lock, and only the thread declares), and no object owned them. Two things looked like they forced free functions, and only one does: `Thread::spawn()` and `GKTL_INIT_HOOK` take plain function pointers. A function pointer is no reason to scatter the state: a lambda that captures nothing converts to one, takes the object as its `void *` argument, and calls the member. The rest copied the shape of the gate in `pmm.cpp`, which has the same fault (the next change).

Being an object pays back in checking: members can say `TA_GUARDED(m_lock)`, which `-Wthread-safety -Werror` checks on every access; the globals said nothing, so the analysis checked nothing. And `Hold` is now a scoped capability (`TA_SCOPED_CAPABILITY`, acquiring the watchdog's lock in its constructor and releasing it in its destructor), where it used to switch the analysis off.

**Change**
Before, the state:

```cpp
namespace {
    sync::Event SIGNAL{sync::Event::Options::AutoUnsignal};
    sync::Mutex LOCK;
    MemoryPressureLevels LEVELS{};
    bool SKIP_HYSTERESIS = false;
    std::atomic<MemoryPressure> PRESSURE{MemoryPressure::Normal};
    auto watchdog_routine(void *) -> i32 { /* declared, declared_at, first as locals */ }
}
```

After:

```cpp
class MemoryWatchdog {
    // ...
  private:
    auto run() -> i32;
    auto look() -> ustl::Option<time::Deadline>;

    sync::Event m_signal{sync::Event::Options::AutoUnsignal};
    mutable sync::Mutex m_lock;
    MemoryPressureLevels m_levels TA_GUARDED(m_lock){};
    bool m_skip_hysteresis TA_GUARDED(m_lock) = false;
    std::atomic<MemoryPressure> m_pressure{MemoryPressure::Normal};
    MemoryPressure m_declared TA_GUARDED(m_lock) = MemoryPressure::Normal;
    time::SteadyClock::TimePoint m_declared_at TA_GUARDED(m_lock){};
    bool m_declared_once TA_GUARDED(m_lock) = false;
};

auto *const thread = task::Thread::spawn(
    "memory-watchdog", [](void *const self) -> i32 { return static_cast<Self *>(self)->run(); },
    this, task::DEFAULT_PRIORITY);
```

`pressure_for(levels, free)` and `bounds_for(levels, pressure)` became `MemoryPressureLevels::pressure_at()` and `bounds_of()`, with `delay_level()` beside them, and `boot_levels()` became `MemoryPressureLevels::from_boot_options()`: they ask the levels, and hold nothing. The one instance is a function-local static in `get()`, as `SparseMemMap::get()` has it. The init hook stays a free function, for the same reason as the thread's entry, and only calls `MemoryWatchdog::get().start()`.

**Reference**
Zircon's `MemoryWatchdog` (`zircon/kernel/object/memory_watchdog.cc`) is a class whose members hold the same state (`mem_watermarks_`, `mem_event_idx_`, `hysteresis_seconds_`), with `WorkerThread()` a member.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- `mempressure.test` ok, 15 checks, with the same transitions in the log; `largepage.test` and `pagesource.test` ok.
- The two files compile with `-Wthread-safety -Werror` (from the compile database) with no warning; clang-tidy reports nothing in them; clang-format clean.

**Deliberately not done**
- The gate in `pmm.cpp`: the next change.

**How it was found**
The user's review of step 3, 2026-10-29.

**The user's prediction**
None

**Three questions you should be able to answer**
- What did a plain function pointer actually force, and how does a lambda that captures nothing get from it back to the object?
- Which checks did the globals lose that the members get back?
- Why are `pressure_at()` and `bounds_of()` methods of the levels and not of the watchdog?

## Title: the PMM's gate for allocations that can wait is an object, FreeMemoryGate, behind the same free functions (2026-10-29)

**Files**
- `kernel/mm/pmm.cpp` — the gate's state, its free-memory signal and their lock are members of `FreeMemoryGate`, one instance in the file; `try_alloc_frames()`, `wait_for_frame_allocation()`, `set_free_memory_signal()` and every allocation and free entry ask it

**Symptom**
No change in behaviour. The same review as the change before: the gate of step 1 kept its state (the state, the delay level, the bounds, the watcher's event, the waiters' event, the watcher's lock) as globals in an anonymous namespace, with the functions that guard it beside them.

**Cause**
The same rule of `modern-cpp-development`, and a sharper case of it: the gate's invariant is spread over six variables that must change together (the bounds before the event that publishes them, the level and the state with them, all under the watcher's lock), and no object said so. Now one type holds all six and is the only code that touches them; the free functions keep `pmm.hpp`'s interface, which is free functions throughout, and forward to it.

The atomics stay: every allocation and free asks the gate something, and the allocation path takes no lock for it. Atomics as members cost the same as atomics at namespace scope.

**Change**
Before:

```cpp
constinit std::atomic<GateState> GATE{GateState::Open};
constinit std::atomic<usize> DELAY_LEVEL{0};
constinit std::atomic<usize> SIGNAL_LOWER{0};
constinit std::atomic<usize> SIGNAL_UPPER{0};
constinit std::atomic<sync::Event *> SIGNAL_EVENT{nullptr};
sync::Event MAY_ALLOCATE{sync::Event::Options::AutoUnsignal};
sync::SpinLock WATCHER_LOCK;
auto check_free_memory_signal() -> void;
auto admit_waitable() -> Status;
```

After:

```cpp
class FreeMemoryGate {
  public:
    auto admit() -> Status;
    auto check_signal() -> void;
    auto wait_for_opening(time::Deadline const &deadline, bool interruptible) -> Status;
    auto pass_on() -> void;
    auto set(usize lower, usize upper, usize delay_level, sync::Event *event,
             DelayedAllocations delayed) -> bool;

  private:
    enum class State : u8 { Open, Shut, Refusing };
    std::atomic<State> m_state{State::Open};
    std::atomic<usize> m_delay_level{0};
    std::atomic<usize> m_signal_lower{0};
    std::atomic<usize> m_signal_upper{0};
    std::atomic<sync::Event *> m_signal_event{nullptr};
    sync::Event m_may_allocate{sync::Event::Options::AutoUnsignal};
    sync::SpinLock m_watcher_lock;
};

FreeMemoryGate GATE;
```

The one instance is a global object, not a function-local static as the watchdog's: every allocation and free reaches it, and a function-local static would add a check of its guard to each. Global objects are built first in `start_kernel()` (`ccrt::init_global_static_objects()`), before the PMM exists. `assert_cannot_wait()` stays a free function: it holds no state.

**Reference**
Zircon keeps the same state as members of `PmmNode` (`should_wait_`, `should_wait_free_pages_level_`, `may_allocate_evt_`, `mem_signal_`, `mem_signal_lower_bound_`, `mem_signal_upper_bound_`, in `zircon/kernel/vm/include/vm/pmm_node.h`).

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- `mempressure.test` ok, 15 checks; `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test` ok.
- `pmm.cpp` recompiles with `-Wthread-safety -Werror` with no warning; clang-tidy reports nothing in it; clang-format clean.

**Deliberately not done**
None

**How it was found**
The user's review of step 3, 2026-10-29.

**The user's prediction**
None

**Three questions you should be able to answer**
- Which six pieces of state make up the gate, and which order of writes between them does `set()` keep?
- Why is this instance a global object while the watchdog's is a function-local static?
- Why do the public functions stay free functions while the state moves into a class?

## Title: PmZone walks every zone itself, and the walks over every node's own zones use it (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `PmZone::for_each()`, over a list of every zone made, which the constructor appends to
- `kernel/mm/pmm.cpp` — `free_frame_count()` and `approx_free_frame_count()` use it
- `kernel/mm/pm_node_init.cpp` — `PmNode::setup_watermarks()`'s two walks use it

**Symptom**
No change in what is counted. The user asked, 2026-10-29, why `approx_free_frame_count()` walked the online nodes and each node's own zones by hand, rather than every zone directly.

**Cause**
The count was right: `ZoneList::for_each_local_zone()` walks the node's by-type array, its own zones only, so each zone was counted once. But "every zone in the machine" had no name, so each caller spelled it out as two loops, four times (the two counts and the two walks of `setup_watermarks()`), and each carried a comment warning that a node's fallback order lists the other nodes' zones too and would count them once per node. A rule every caller must remember belongs in the one place that walks.

Linux's `for_each_zone()` (`include/linux/mmzone.h:1788`) is the same two loops, online nodes then each node's `node_zones`, hidden in `next_zone()` (`mm/mmzone.c:30`). Here the zone itself keeps the list, as the user asked: a zone is made once per node and type while the PMM is brought up on the boot CPU, and never destroyed, so a list its constructor appends to is complete before anything walks it, and the walk needs neither a lock nor PmNode.

**Change**
Before, in each of the four:

```cpp
PmNode::for_each_nid(PmNode::State::Online, [&total](NnId const nid) {
    auto *const node = PmNode::node(nid);
    if (!node) {
        return;
    }
    node->zone_list()->for_each_local_zone(
        [&total](PmZone &zone) { total += zone.free_frames(); });
});
```

After:

```cpp
PmZone::for_each([&total](PmZone &zone) { total += zone.free_frames(); });
```

with, in `PmZone`:

```cpp
template <typename F>
    requires ustl::traits::Invocable<F, PmZone &>
static auto for_each(F &&f) -> void {
    for (auto *const zone: ZONES) {
        f(*zone);
    }
}

static inline ustl::collections::StaticVec<PmZone *, NUMA_MAX_NODE * NR_ZONES_PER_NODE> ZONES;
```

The walks that are about one node stay on its ZoneList: the memmap's per region, the fallback order a node builds from the others' zones, and a node's own dump.

**Reference**
Linux `for_each_zone()` and `next_zone()`, cited above. For the free count itself Linux walks nothing: `global_zone_page_state(NR_FREE_PAGES)` (`include/linux/vmstat.h:176`) reads one counter, `vm_zone_stat[]`; that is the direction of `docs/repairs/261029/mm.md` item 1, still to be measured.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- The zones' watermarks at boot are the same as the boot before this change (DMA 19/23/27, DMA32 690/862/1034). DMA32 manages 2 frames fewer (122829 against 122831), because the kernel image grew by a page (420 to 421 pages) and the package carrying it by one (451 to 452 frames): the list of zones and the code crossed a page boundary. The walk changes no total.
- `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test`, `pagesource.test` ok.
- clang-tidy: nothing on the lines changed (one finding, `lists_` at `pm_zone.hpp:26`, is on a line this did not touch); clang-format clean.

**Deliberately not done**
- A running total of free frames, which would make `approx_free_frame_count()` one load: a performance change, `docs/repairs/261029/mm.md` item 1.
- The unmerged `feat/pmm-cpp-vocabulary` rewrites `pm_zone.hpp` and `pm_zone.cpp` too; merging it will meet this in both files.

**How it was found**
The user's review, 2026-10-29.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why did the old walks give the right count, and what mistake was each caller one step from?
- Why can the list of zones be read without a lock?
- Which walks over zones are about one node, and why do they stay on the node's ZoneList?

## Title: the platform hands the PMM one zone bound per zone type, by type, not a short list by position (2026-10-29)

**Files**
- `kernel/platform/pc/memory.cpp` — `platform_init_pmm_with_config()` fills an array of `NR_ZONES_PER_NODE` bounds, each at its type's index, 0 for a type the platform does not list
- `kernel/mm/pm_node_init.cpp` — `set_zone_bounds()` asserts one bound per type
- `kernel/mm/include/ours/mm/init.hpp` — `init_pmm()` says what its argument is; the comment it had described a parameter that does not exist
- `docs/repairs/261029/mm.md` — item 2 removed: this change

**Symptom**
Undefined behaviour on every boot, with no effect only by luck. `set_zone_bounds()` loops five times, once per zone type, over the bounds the platform passes, and x86 passed three (DMA, DMA32, Normal, `mem_cfg.hpp:23`), so entries 3 and 4 were read from past a `std::array` on the caller's stack (`std::span`'s `[]` checks nothing). They became the upper bounds of Movable and Dynamic, whose lower bound is Normal's end, `max_pfn`, so every node's range cut to nothing against them and no such zone was made.

And a latent mislabelling: with DMA switched off (`OURS_CONFIG_ZONE_DMA` 0), the old code reported `Zone[DMA]: managed 126280 frames` on the dev image: all its memory called DMA, the type that was switched off and that can only be the lowest 16 MiB.

**Cause**
One cause for both. The platform's list holds only the types it has, a switched-off type is filtered out of it (`DisabledZoneFilter`), and `platform_init_pmm_with_config()` copied what was left into an array by position. The kernel read that array by type. So the array was shorter than the types, and read past its end; and a type left out moved every later bound onto the type before it, which with DMA off put DMA32's bound on DMA and Normal's on DMA32.

**Change**
Before:

```cpp
std::array<mm::Pfn, sizeof...(LinearZoneMaxPfns)> linear_zone_pfns;
auto index = 0;
// ... for each listed type:
linear_zone_pfns[index++] = std::min(Pfn, mm::max_pfn());
```

After:

```cpp
std::array<mm::Pfn, mm::NR_ZONES_PER_NODE> max_zone_pfns{};
// ... for each listed type:
max_zone_pfns[static_cast<usize>(Type)] = std::min(Pfn, mm::max_pfn());
```

A 0 lays the type out empty where the types before it end, since `set_zone_bounds()` takes `max(bound, start)`. `set_zone_bounds()` now asserts it was given one bound for each type.

Rejected: only bounding the loop by the list's length, which was the plan as first written. It ends the read past the end but keeps the positions, so the shift with a type left out would stay.

**Reference**
None, this project's own code.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed. The zones in the dbg boot log the same as before (DMA 3451 frames, watermarks 19/23/27; DMA32 122829, 690/862/1034). The rel image's zones were not recorded before the change, so they are not compared.
- `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test` ok.
- Control: a list of three handed to `init_pmm()` again stops the boot on the new assertion ("3 zone bounds for 5 zone types").
- DMA switched off, both versions booted: the old code reports `Zone[DMA]: managed 126280 frames`, the new one `Zone[DMA32]: managed 126280 frames`. Restored after.
- clang-tidy: nothing in the files changed; clang-format clean.

**Deliberately not done**
- Repair item 3 (the per-type node states): step 2 of `.agents/tasks/zone-table` removes them (D3).

**How it was found**
Reading `set_zone_bounds()` to move it into the zone table (`.agents/tasks/zone-table`), and then the platform's caller and `std::span`'s index.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why did reading two values past the array have no visible effect, and what would have made it show?
- Why does a type left out of a list kept by position move every later bound, and onto which type?
- Why does a 0 bound give an empty zone rather than a zone from the bottom of memory?

## Title: the zones are cut into a table before any node exists, and each node takes its row (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `PmZone::create_all()` cuts every node's memory into zones and fills `TABLE`, nodes by types; `PmZone::at(nid, type)` reads a cell; `for_each()` walks the table; the list the constructor appended to (`3c11c310`) is gone
- `kernel/mm/pm_node_init.cpp` — `bootstrap()` calls `create_all()` before `create_nodes()`; `PmNode::init()` makes no zone and takes the ones in its row; the zone bounds and their globals moved to `create_all()`
- `kernel/mm/include/ours/mm/pm_node.hpp` — the per-type states `State::Dma`, `Dma32`, `Normal` removed (D3)
- `docs/repairs/261029/mm.md` — item 3 removed: this change

**Symptom**
None: step 2 of `.agents/tasks/zone-table/PLAN.md`. Each node made its own zones in `PmNode::init()`, so "every zone" had no owner but a side list kept in step by the constructor, and each node recorded which types it had a second time, as per-type states that nothing read and whose numbers would have set `Memory` for a Movable zone and `Online` for a Dynamic one.

**Cause**
The rule that divides memory into zones is static, and what it divides, each node's range, is in the boot memory map before `init_pmm()` runs; a zone needs its node's number and nothing of its `PmNode`. So the zones can be cut first, into one place that is the only record of them, and a node takes what is there. The table is a fixed grid (D1): `NUMA_MAX_NODE` rows by `NR_ZONES_PER_NODE` columns of pointers, at most 160, null where a node has no memory of a type. Whether a node has memory of a type is then a cell, not a state (D3).

**Change**
`bootstrap()`, before:

```cpp
set_zone_bounds(max_zone_pfn);
// ...
create_nodes(nodes);
nodes.for_each([](NnId const nid) { PmNode::node(nid)->init(); });  // each cuts its zones
```

After:

```cpp
// ...
PmZone::create_all(max_zone_pfn, nodes);  // every zone, into PmZone::TABLE[nid][type]
create_nodes(nodes);
nodes.for_each([](NnId const nid) { PmNode::node(nid)->init(); });  // each takes its row
```

and in `PmNode::init()`, the loop that cut, allocated and constructed zones became:

```cpp
for (usize i = 0; i < NR_ZONES_PER_NODE; ++i) {
    auto const ztype = ZoneType(i);
    if (auto *const zone = PmZone::at(m_id, ztype)) {
        zone_list()->insert_local_zone(zone, ztype);
        total_present_frames += zone->present_frames();
    }
}
```

Each zone is still allocated from its own node's memory (`EarlyMem::allocate<PmZone>(1, nid, ...)`). The test that dropped a cut outside the node's range (`zone_start_pfn < node_start_pfn || zone_end_pfn > node_end_pfn`) is gone: a range clamped to a type's bounds that falls outside it comes out with its end at or below its start, which the remaining test already skips. `TABLE` is `constinit` and holds only pointers, so it needs no constructor.

The node's `ZoneList` still keeps its own zones by type, filled from its row; step 3 has it read the table instead.

**Reference**
None for the table: this project's own design (`.agents/tasks/zone-table/DESIGN.md`). Linux keeps each node's zones inside its node structure (`pg_data_t::node_zones`) and walks the nodes to reach them (`for_each_zone()`, `next_zone()`); here the zones come first and the node takes them.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed. The zones in the dbg boot log the same as before the step (diffed).
- A build with `mm_numa = true` (`out/zone-table.numa`), booted with `NUMA=1` as four nodes of 1 GiB, before and after: both images PASS, and the four nodes' fallback orders and the five zones (`Zone[...]: managed ... watermarks ...`) the same, line for line (diffed).
- `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test`, `pagesource.test` ok on the default build; the first four ok on the four-node build.
- Control: `create_all()` leaving the DMA32 cell empty stops the boot ("block [0x1000, 0x1400) is outside the span [0x0, 0x1000)": DMA32's frames, with no zone of their own, were handed to the DMA zone). Restored after.
- clang-tidy: nothing on the lines changed (the findings it reports in `pm_node.hpp`, `modernize-use-nodiscard` on lines 74, 102, 108, 175 and 180, are on lines this did not touch); clang-format clean.

**Deliberately not done**
- The node's `ZoneList` keeping a copy of its row, `PmZone::m_node` (D2), and the free path's lookup through the node: step 3.

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- What does cutting a node's memory into zones need to know, and why does that let it come before the node exists?
- Why can the table be read with no lock, and what would a zone added later have to do to keep that true?
- Why did leaving one cell empty stop the boot rather than just lose that memory?

## Title: a node's ZoneList reads its own zones from the zone table, and a frame finds its zone there directly (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/pm_node.hpp` — `ZoneList` keeps the fallback order only: its by-type array and `insert_local_zone()` are gone, `local_zone()`, `local_zones()` and `for_each_local_zone()` read the node's row of the table, and `take_local_zones()` puts that row at the head of the fallback order
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `PmZone::row(nid)`
- `kernel/mm/pm_node_init.cpp` — `PmNode::init()` fills each of its zones' `m_node` (D2) and takes its row
- `kernel/mm/pm_node.cpp`, `kernel/mm/pmm.cpp`, `kernel/mm/memmap_init.cpp` — a frame's zone is `PmZone::at(frame->nid(), frame->zone())`, not found through its node
- `kernel/mm/pm_zone.cpp` — the comment on `m_node` says who fills it

**Symptom**
None: step 3 of `.agents/tasks/zone-table/PLAN.md`. After step 2 the same zones were recorded twice, in the table and in each node's `ZoneList` by type; and `PmZone::m_node`, which a comment said the node filled in when it adopted the zone, was only ever null.

**Cause**
A copy is one fact with two owners, and the next change to one of them is the day they disagree. The table is the record of which node has which zone; the node's list now only orders them for its allocations, which is the one thing that is the node's own. The free path went through the node only to reach its copy: `PmNode::node(frame->nid())` then its list then the type; the frame's two numbers find the zone in the table with no node at all.

**Change**
Before, in `PmNode::free_frames()`:

```cpp
auto const node = PmNode::node(frame->nid());
auto const zone = node->m_zone_list.local_zone(frame->zone());
DEBUG_ASSERT(zone, "");
```

After:

```cpp
auto *const zone = PmZone::at(frame->nid(), frame->zone());
DEBUG_ASSERT(zone, "frame {:#x} is in no zone", frame_to_phys(frame));
```

`ZoneList`, before: `std::array<ZoneRef, NR_ZONES_PER_NODE> m_zones` beside the fallback order, filled by `insert_local_zone()`. After: `local_zone(type)` is `PmZone::at(m_nid, type)`, `local_zones()` is `PmZone::row(m_nid)`, and `PmNode::init()` ends with:

```cpp
for (auto *const zone: PmZone::row(m_id)) {
    if (zone) {
        zone->m_node = this;
        total_present_frames += zone->present_frames();
    }
}
m_zone_list.take_local_zones();
```

`local_zone()` and `local_zones()`, rewritten here, are `[[nodiscard]]` now.

**Reference**
None, this project's own design (`.agents/tasks/zone-table/DESIGN.md`).

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed. The zones in the dbg boot log the same as before the task (diffed against this build's own log; see How it was found).
- The `mm_numa = true` build, booted as four nodes: both images PASS; the four fallback orders and four of the five zones the same, line for line; node 3's Normal zone manages 62890 frames instead of 62892, because the kernel image grew from 427 to 428 pages and the package carrying it from 454 to 455 frames, both on that node; its watermarks unchanged.
- `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test`, `pagesource.test` ok on the default build; the first four ok on the four-node build.
- Control: `PmZone::at()` answering null for DMA32 stops the boot on the free path's assertion ("frame 0x1000000 is in no zone"). Restored after.
- clang-tidy: nothing on the lines changed (`modernize-use-nodiscard` on `fallback()`, `begin()` and `end()` in `pm_node.hpp` stays, on lines this did not touch); clang-format clean.

**Deliberately not done**
- `PmZone::m_node` has no reader yet; it is filled because a field kept for planned work is not deleted as cleanup (D2).

**How it was found**
One comparison first showed three extra zone lines in the boot log. The log's first line named the kernel of `.worktrees/feat-pmm-layering`: `boot_check.py` keeps its logs in a directory named after the build directory's last component, and that worktree's session boots an `out/bringup.x86` too, so its boot had overwritten this one's log. Booted again, with the first line checked, the zones were the same. The tool's collision is recorded in `docs/repairs/261029/ours-verify.md`.

**The user's prediction**
None

**Three questions you should be able to answer**
- After step 2, which fact had two owners, and which one is the node's own now?
- Why does the free path no longer need the node, and what did it cost to go through it?
- Why is `m_node` filled although nothing reads it?
