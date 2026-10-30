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

## Title: every VMO page is filed on its node's page queue for its kind, with a backlink to the node that holds it (2026-10-30)

**Files**
- `kernel/mm/include/ours/mm/page_queues.hpp`, `kernel/mm/page_queues.cpp` — the anonymous zero-fork queue, `set_owner()`, `kind_of()`, `count()`
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `file_page_locked()`, `own_all_pages_locked()`, `set_page_owner()`, `unfile_page()`, `free_page()`, called wherever a page enters a node, moves between nodes or leaves; `Flags::Pinned`
- `kernel/mm/vm_object_paged.cpp` — a pinned VMO's node is made with `Flags::Pinned`
- `kernel/mm/include/ours/mm/pm_node.hpp` — `PmNode::page_queues()`
- `kernel/mm/pmm.cpp` — `free_frames()` asserts a VMO page is on no queue
- `kernel/commands/page_queues_test.cpp`, `kernel/commands/BUILD.gn` — `pagequeues.test`, new

**Symptom**
None: step 1 of `.agents/tasks/page-reclaim/PLAN.md`. `PageQueues` had its lists and its generations, and no page was ever put on them: every count was zero for the life of the kernel, and the backlink fields of the frame descriptor were never written.

**Cause**
Why it is shaped as it is:

- **Filed where the page goes in, under the node's lock.** A page is filed right after it enters the node's page list, so no one who takes the node's lock sees a page in the list and on no queue. The kind is the node's to know: a pinned node files pinned, a node with a page source files reclaimable, and any other files anonymous, zero-fork when the page was committed in place of zero content (a write to a hole or a zero marker, the pages of a 2 MiB block) and plain anonymous when it came with content (a copy, a supply, wired boot pages).
- **Moves change the owner, not the queue.** A clone's pages go whole to its new hidden node, a hidden node's page to the leaf that takes it, the pages of a merged hidden node to its surviving child. The page keeps its queue; only the backlink changes, under the queue lock, which a reclaimer will read it under (`set_owner()`).
- **Off the queue before every free.** A frame has one list hook, and the free lists use it too: a page freed while filed would stitch its queue and a free list together. So every free of a VMO page goes through `free_page()`, which takes the page off first, and `free_frames()` asserts it in debug builds. A page taken out of a node for a list of pages (`take_pages_locked()`) is taken off too, and filed again where it is supplied.
- **The destructor takes the pages off under the queue lock.** That is the half of the lifetime rule of the design that this step owns: when a reclaimer reads a page's owner under the queue lock, the owner's storage cannot be gone yet, because its destructor would still be waiting for that lock to take the page off.
- **Pinned is told to the node.** `VmoFlags::Pinned` belonged to `VmObjectPaged`, but the node files the pages; `VmCowPages::Flags::Pinned` carries it down.
- **The zero-fork queue is new.** It holds the anonymous pages likely to be zero again, the only ones the zero check of step 5 will look at (D1, separate queues for each kind).

**Change**
The helper every insertion calls:

```cpp
auto VmCowPages::file_page_locked(VmPage *const page, PgOff const index, Fill const fill) -> void {
    auto &queues = queues_of(page);
    if (!!(m_flags & Flags::Pinned)) {
        queues.set_pinned(page, this, index);
    } else if (m_page_source) {
        queues.set_reclaimable(page, this, index);
    } else if (Fill::ZeroFork == fill) {
        queues.set_anonymous_zero_fork(page, this, index);
    } else {
        queues.set_anonymous(page, this, index);
    }
}
```

`queues_of(page)` is the queues of the node the frame is on (`PmNode::node(page->nid())`), so a page is filed with the memory it is, wherever its VMO is used from. The destructor, before: `m_pages.drain_owned([](VmPage *const page) { free_frames(page, 0); });`; after: `free_page(page)`, which takes it off first.

**Reference**
Zircon `PageQueues::SetAnonymous()`, `SetAnonymousZeroFork()`, `SetReclaim()` and `SetWired()` with a `VmCowPages` backlink and offset (`zircon/kernel/vm/include/vm/page_queues.h`), called by `VmCowPages` as pages enter it, and `PageQueues::Remove()` before a page is freed.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 151 passed. Lockdep, on in dbg, reports nothing for the new nesting of the queue's spinlock inside a node's lock.
- `pagequeues.test`: ok, 7 checks (zero-fork, the hand-over to the hidden node, the clone's copy, the last viewer taking the page back, pinned, reclaimable, every count back where it was). `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`, `mempressure.test`, `vmaundo.test`, `usercopy.test`, `pmnode.test`, `pmzone.test` ok.
- The `mm_numa = true` build booted as four nodes: both images PASS; `pagequeues.test`, `vmosnapshot.test`, `largepage.test`, `pagesource.test` ok.
- Controls, each restored after: the destructor freeing its pages without taking them off stops the boot on the new assertion in `free_frames()` (a VMO is destroyed during boot); the clone not handing its pages' backlinks to the hidden node fails the test's hand-over check.
- clang-tidy: nothing on the lines changed; clang-format clean.

**Deliberately not done**
- Nothing rotates the ring or reads the queues yet: steps 2 to 4.
- `pagequeues.test` compares node 0's counts; on the four-node build the pages it makes may be on another node, so there it checks each page's kind and owner, not the counts' return.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why must a page come off its queue before it is freed, and what would a page freed while filed do to the free list?
- Why does a page that moves from one node to another keep its queue and change only its backlink?
- How does taking the pages off in the destructor, under the queue lock, make a reclaimer's `try_adopt()` safe?

## Title: each node's reclaimable pages age on a thread every few seconds, and a page the kernel hands out is marked used (2026-10-30)

**Files**
- `kernel/mm/include/ours/mm/page_queues.hpp`, `kernel/mm/page_queues.cpp` — `start_runners(period)` starts the aging thread, `rotate()`, `age_of()`, `rotations()`, `next_rotation_due()`, `to_string(AgeReason)`; the dead `m_min_rotate_time`, `m_max_rotate_time`, `m_last_age_reason` removed; `kind_of()` takes the queue lock; an init hook starts a thread per online node
- `kernel/include/ours/boot/options.hpp`, `kernel/top/boot_options.cpp` — `kernel.mm.page-aging-seconds`, 5 by default, 0 taken as 1
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `Cursor::disable_mark_accessed()`, and `handed_out()` at every exit of `require_owned_page()`, `require_read_page()` and `maybe_page()`
- `kernel/mm/vm_mapping.cpp` — a populate and a hardware fault turn the mark off
- `kernel/mm/include/ours/mm/vm_fault.hpp` — `VmfCause::Software` has a reader now
- `kernel/commands/page_queues_test.cpp` — four checks: the thread rotates, and a supplied page ages and is made the newest again by a read
- `.agents/skills/ours-kernel-design/references/mm.md` — page aging is built, but for the harvest

**Symptom**
None: step 2 of `.agents/tasks/page-reclaim/PLAN.md`, and `docs/repairs/261026/mm.md` item 9. Nothing rotated the ring: `start_runners()` had a body of comments and no caller, so a page filed in the newest generation (step 1) stayed there for the life of the kernel, and every page looked as young as every other.

**Cause**
Why it is shaped as it is:

- **A thread sleeps to a deadline, not for an interval.** The next rotation is due a period after the last one actually happened (`next_rotation_due()`), so the cadence does not drift by however long a rotation took. A manual `rotate()` moves the deadline too, and the thread, waking at the old one, sees it moved and sleeps again rather than rotating twice in a row. Before, `m_last_age_time` was the clock's epoch until the first rotation, so a thread that started would have rotated at once; `start_runners()` sets it to now, so a page filed during boot is as young as the clock says.
- **The mark is on by default, at every exit.** Every page `Cursor` hands out goes through `handed_out()`, so a lookup added later marks too. A caller whose access goes through a page table entry turns it off, because the entry's accessed bit records the access: a populate (`map_paged()`), and a fault the hardware raised. A fault the kernel raised for a user copy (`VmfCause::Software`) marks, though its retry goes through the entry as well; twice costs nothing. Default on because a forgotten mark gets a page in use reclaimed, while a forgotten opt-out only marks twice (the rule decided 2026-10-25).
- **`kind_of()` now takes the queue lock.** Step 1 read the queue byte under the VMO's lock alone, which was enough while only the VMO's code wrote it. The aging thread now rewrites it, under the queue lock alone, for every page of the oldest generation when the ring is full (`advance_lru_locked()`). A VMO lock and a queue lock do not exclude each other, so the read and that write could run at once: a data race on a plain `u8`, undefined behaviour however harmless a byte read looks on x86. The same is why `handed_out()` does not check the kind itself before calling `mark_accessed()`: it would read the same byte without the lock.
- **One thread per node, from an init hook after Threading.** Each node's queues are its own, so a node's pages age on a thread of their own, as the memory watchdog's thread is started from the same level.

**Change**
The loop, which was there but never run, now reads its deadline from the queues and checks it again on waking:

```cpp
auto const next = m_owner->next_rotation_due();
auto _ = task::Thread::Current::sleep_until(time::Deadline::at(next), task::Interruptible::Yes);
...
// A manual rotation may have moved the deadline while this slept.
if (time::SteadyClock::now() < m_owner->next_rotation_due()) {
    continue;
}
m_owner->rotate_reclaimable_queues(AgeReason::Timeout);
```

The mark, before at no exit, now at every one:

```cpp
// before
return ktl::ok(RequireResult{.page = found.page, .writable = true});
// after
return ktl::ok(handed_out({.page = found.page, .writable = true}));
```

and turned off where the hardware records the access:

```cpp
if (!(vmf->cause & VmfCause::Software)) {
    cursor->disable_mark_accessed();
}
```

`start_runners()` returns `Status`, and the thread is spawned with a captureless lambda that finds its runner, as the skill's rule for C callbacks asks.

**Reference**
Zircon `PageQueues::StartThreads()` and `PageQueues::MruThread()` with `RotateReclaimQueues()` (`zircon/kernel/vm/page_queues.cc`), whose MRU thread rotates on a minimum period. The mark: Zircon `VmCowPages::LookupCursor::DisableMarkAccessed()`, called by `VmMapping::MapRange()` (a populate), `VmMapping::PageFaultLockedObject()` (a fault through a mapping) and `VmObjectPaged::GetPage()` for `VMM_PF_FLAG_HW_FAULT`. Zircon's `MapRange()` and `PageFaultLockedObject()` turn it off unconditionally, since its soft faults take `GetPage()`; here soft faults take the mapping's fault path, so it asks the cause.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped); host tests 151 passed. The dbg log shows `mm: node 0 ages its pages every 5 s`.
- `pagequeues.test`: ok, 11 checks; the thread check waits a period and a second. `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`, `mempressure.test`, `vmaundo.test`, `usercopy.test`, `pmnode.test`, `pmzone.test` ok.
- The `mm_numa = true` build as four nodes: a thread per node in the log, `pagequeues.test`, `pmnode.test`, `mempressure.test` ok.
- `kernel.mm.page-aging-seconds=0` on the kshell image: the log says every 1 s, and the thread check passes in 2 s instead of 6.
- Controls, in one build, restored after: the mark off and the thread not started fail exactly "a read through the VMO makes it the newest again" and "the aging thread rotates within a period", 2 of 11.
- clang-tidy: nothing on the lines changed; clang-format clean.

**Deliberately not done**
- The harvest of the accessed bit: step 3. Until then a page used only through a mapping looks unused, which costs a refetch, never wrong content, and nothing reclaims yet anyway.
- `request_shutdown()` has no caller: a node's queues live as long as the kernel.
- The lock `mark_accessed()` takes for every page handed out, even one with no age: recorded as `docs/repairs/261030/mm.md` item 1, to measure before changing.
- `MruRunner` stays an empty scaffold; the loop is on `LruRunner` until a reclaimer gives the LRU side work of its own.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why does the aging thread check the deadline again after it wakes, and what would happen without that check after a manual `rotate()`?
- Why is a hardware fault's lookup not marked, while a soft fault's is?
- Why did the aging thread make the unlocked read in `kind_of()` wrong, when it was right in step 1?
