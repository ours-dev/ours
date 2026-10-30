# zone-table: the zones are cut into a table before any node exists, and each node takes its row of it

**Date**: 2026-10-30

## Delivered
`PmZone::create_all()` cuts every node's memory into zones while the PMM is brought up, before any `PmNode` is made, and puts each in `PmZone::TABLE`, a fixed grid of nodes by zone types; each zone's structure is still allocated from its own node's memory. A node takes its row: its `ZoneList` keeps only the fallback order and reads its own zones from the table, and a frame finds its zone by its node and type with no node in between. The table is the one record of which node has which zone, and `PmZone::for_each()` walks it. The design is `docs/design/261029/zone-table.md`; the plan is `PLAN.md` here.

- `7038b64e` docs(task): the design, proposed, after a draft with diagrams the user read first.
- `225e47f0` docs(repairs): two problems found while reading the code the table replaces (zone bounds read past the platform's list; node states numbered after zone types).
- `6e1cf752` docs(task): D1 to D3 decided, the plan in three steps.
- `b1d99016` fix(mm): step 1, the platform hands the PMM one zone bound per zone type, by type.
- `34127a7b` refactor(mm): step 2, the table, made before the nodes; the per-type node states removed (D3).
- `008fb0c6` refactor(mm): step 3, the node reads its row from the table; `PmZone::m_node` filled (D2); the free path looks a frame's zone up directly.
- `2a1b292b` docs(repairs): the log collision of `boot_check.py` across worktrees, seen happen during step 3.
- This commit: this document, and the design moved to `docs/design/261029/zone-table.md`.
- Records: `docs/changes/261029/mm.md`, three entries. The task replaces the list of zones that `3c11c310` had the constructor keep.

## Verification
At the final state (`008fb0c6`):

- `boot_check.py`, default build: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed. The zones in the dbg boot log the same as before the task, diffed (DMA 3451 frames, watermarks 19/23/27; DMA32 122829, 690/862/1034).
- A build with `mm_numa = true` (`out/zone-table.numa`), booted with `NUMA=1` as four nodes of 1 GiB: both images PASS before the task and after each step. The four nodes' fallback orders and the five zones the same, diffed, except node 3's Normal zone at 62890 frames instead of 62892 after step 3: the kernel image grew a page (427 to 428) and the package carrying it a frame (454 to 455), both on that node; its watermarks unchanged.
- `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test` pass on both builds; `pagesource.test` on the default one.
- Controls, each restored after: a list of three bounds handed to `init_pmm()` stops the boot on the new assertion; with DMA switched off the old code called all 126280 frames DMA and the new code DMA32; `create_all()` leaving the DMA32 cell empty stops the boot on the buddy's span check; `PmZone::at()` answering null for DMA32 stops it on the free path's new assertion.
- clang-tidy: nothing on the lines changed; clang-format clean on them.
- Not measured: the free path's lookup through the table against the old one through the node. It does less (no `PmNode::node()`, one array index), but no number says by how much.

## Deviations from the design
- **Step 1 fixed the cause, not the loop.** The plan bounded the loop by the platform's list. Reading the platform's side showed the list was copied by position after switched-off types were filtered out, so a type left out shifted every later bound; the platform now fills one bound per type, by type, and the kernel asserts it has all of them.
- **A redundant test went in step 2.** `PmNode::init()` dropped a cut outside the node's range explicitly; clamped to a type's bounds, such a cut comes out empty, which the remaining test already skips.
- **`ZoneList` kept its three accessors** (`local_zone()`, `local_zones()`, `for_each_local_zone()`) as views of the table rather than removing them, so the walks that are about one node, and the tests, did not change.
- **`PmZone::m_node` has no reader.** It is filled (D2) because a field kept for planned work is not deleted as cleanup.

## Found along the way
- **`set_zone_bounds()` read past the platform's list on every boot, and a switched-off type would have shifted the others**: fixed in step 1.
- **`PmNode::State` numbered three per-type states after zone types**, so a Movable zone would have set `Memory` and a Dynamic one `Online`; nothing read them: removed in step 2.
- **`PmZone::m_node` was never written**, though its comment said the node filled it: filled in step 3.
- **`boot_check.py` and `kshell_run.py` name their log directories after the build directory's last component**, so two worktrees that both build into `out/bringup.x86` overwrite each other's logs; during step 3 a comparison read another worktree's log. Recorded in `docs/repairs/261029/ours-verify.md` item 1. Each comparison here was then checked against the log's first line.
- **The managed frames of a zone move with the kernel image's size**: a change that adds a page of code takes a page or two from the zone holding the image. A comparison of zones across builds compares the watermarks and the image sizes, not only the frame counts.

## Not done
- **Memory hotplug.** The table leaves room for it (a new node fills a row, memory of a new type fills a cell), but a zone that grows needs its `FrameSet` to grow, and a zone that goes needs its readers drained first; neither is designed.
- **A running total of free frames**, which would make the free count one load: `docs/repairs/261029/mm.md` item 1, to be measured.
- **The unmerged `feat/pmm-cpp-vocabulary` and `feat/pmm-layering`** change `pm_zone.hpp`, `pm_zone.cpp`, `pm_node.hpp` and `pm_node_init.cpp` too; merging either meets this task in those files.
- **`modernize-use-nodiscard` on `ZoneList::fallback()`, `begin()` and `end()`** in `pm_node.hpp`, on lines this task did not touch.
- **`out/zone-table.numa`**, the NUMA build directory made for this task, is still there for the next comparison.
