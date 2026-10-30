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
