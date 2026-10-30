# mm changes

## Title: feat/pmm-layering merged into baseline: its node layer, with baseline's zone table, memory gate and zone bounds kept on top (2026-10-30)

**Files**
- 10 files both branches had changed: `kernel/mm/pmm.cpp`, `pm_zone.cpp`, `pm_node_init.cpp`, `memmap_init.cpp`, `BUILD.gn`, `include/ours/mm/pm_zone.hpp`, `pm_node.hpp`, `gaf.hpp`, and the day's `docs/changes/261029/mm.md` and `docs/repairs/261029/mm.md`
- `kernel/platform/pc/memory.cpp` — `std::to_underlying` for the zone type, as the branch writes every enum cast
- `docs/repairs/261028/mm.md` — item 3 removed, fixed by baseline's `b1d99016`
- `kernel/mm/pm_node.cpp` — one blank line the branch left, and `.agents/tasks/numa-out-of-mm/DESIGN.md` and `.agents/tasks/pmm-layering/PLAN.md` — the status line a bare word with its explanation on the next line: the pre-commit hook checks every line the merge changes, the branch's included, and refused these
- `.agents/tasks/memory-pressure/PLAN.md`, `RESULT.md`, `.agents/tasks/zone-table/RESULT.md` — "item 1" of `docs/repairs/261029/mm.md` is item 3 now

**Symptom**
None: a merge. `feat/pmm-layering` (37 commits, holding the 12 of `feat/pmm-cpp-vocabulary`) reworked the node layer from 2026-10-28: the node count a constant `MAX_NODES` in `<ours/node_mask.hpp>` and `NnId` renamed `NodeId`, the NUMA tables a library of their own (`kernel/lib/numa`), `PmNode::Current` dissolved into `NodePlacement` and `local_node_for()`, the fallback order derived outside mm, the zone selector, allocation entry points answering with an Option, and `std::to_underlying` for enum casts. Baseline had meanwhile added the memory gate and watchdog (`.agents/tasks/memory-pressure`) and the zone table (`.agents/tasks/zone-table`) to the same files.

**Cause**
Two sessions changed one layer at once; the merge-base is 2026-10-28, so this is a three-way merge, not the cherry-pick a branch without a common ancestor needs. The rule the user set for conflicts, 2026-10-30: the branch's structure wins, and baseline's features are carried over in its terms.

**Change**
How each conflict went:

- `pm_zone.hpp`, `pm_zone.cpp`: both sides' includes; the table and `create_all()` in the branch's names (`NodeId`, `MAX_NODES`, `<ours/node_mask.hpp>`, `std::to_underlying`).
- `pm_node.hpp`: `ZoneList` without its by-type copy (baseline) on the branch's `NodeId`; `State : u32` (branch) without the per-type states (baseline, D3 of zone-table); `alloc_request.hpp` (branch) in place of `memory_allocation_context.hpp`. The branch's `ZoneList::connect(order)` asserts the order never holds the node itself, so the node's own zones come from `take_local_zones()` alone, as before.
- `pm_node_init.cpp`: the zone bounds and `set_zone_bounds()` gone into `create_all()` (baseline), whose fix of the read past the platform's list the branch had filed as `docs/repairs/261028/mm.md` item 3 and not made; `first_node()` in the branch's names; `PmNode::init()` takes its row, no longer summing present frames, since the branch removed the node's count.
- `pmm.cpp`: the gate's functions (baseline) reach a node through the branch's `local_node_for()`, which takes the flags by reference to apply the thread's policy, so `try_alloc_frames()` hands it a copy without `CanWait`; a forward declaration, since it is defined further down. The allocation wrappers keep the branch's node lookup with baseline's assertion and signal check. `current_node()` is the branch's. `unwire_frame()` and `memmap_init.cpp` find a frame's zone in the table, and keep the branch's check that its node exists, now as a plain null test, the node itself no longer used.
- `gaf.hpp`: `GAF_USER_VMO` (baseline); `AllocationOptions` gone (branch, `33c4a7f3`).
- `BUILD.gn`: both new sources, `memory_watchdog.cpp` and `node_placement.cpp`.
- The day's change record: every entry of both, the branch's first. The day's repair file: the branch's items 1 and 2, baseline's item 1 renumbered 3, and the live documents that cited it updated; change records citing "item 1" keep it, as written at the time.

**Reference**
None, a merge of this project's own branches.

**Verification**
Done in `.worktrees/merge-pmm-layering`, built into `out/merge-pmm.x86` and `out/merge-pmm.numa`, names no other worktree uses (`docs/repairs/261029/ours-verify.md` item 1).

- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 151 passed, the branch's NUMA library tests among them.
- The zones against baseline's: watermarks the same; DMA 3450 frames instead of 3451 and DMA32 122825 instead of 122829, which is the merged kernel's growth exactly: kernel image +2 pages, the package carrying it +2 frames, the boot package +1.
- The `mm_numa = true` build, booted as four nodes: both images PASS; the four fallback orders the same, line for line, though the branch now derives them outside mm; three zones the same, DMA 1 frame fewer and node 3's Normal 6 fewer, again the image growth exactly (boot package +1; kernel image +4, package +2); watermarks the same.
- `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test` ok on the default build; the first four ok on the four-node build.
- clang-tidy on the resolved files: only `modernize-use-nodiscard` on `ZoneList::fallback()` and the naming of `FramePcpuCache::lists_`, on lines neither side touched.

**Deliberately not done**
- The branch's session is still at work in its worktree (the x86 AP trampoline, uncommitted); what it commits after `f8b98987` comes in with a later merge, and merging baseline back into the branch meets these same files there.

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- Why is this a three-way merge and not a cherry-pick of the branch's commits?
- Why does `try_alloc_frames()` hand `local_node_for()` a copy of its flags?
- How do the merged boots' zone counts show that only the kernel's size changed them?
