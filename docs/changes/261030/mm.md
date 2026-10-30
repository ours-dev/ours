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

## Title: the aging thread harvests the accessed bit of every user mapping before it rotates, so a page used through a mapping stays young (2026-10-30)

**Files**
- `kernel/arch/x86/lib/page_tables/include/page_tables/page_tables.hpp`, `mmu_format.hpp` — `X86PageTableImpl::harvest_accessed()`, a range walk that asks a visitor about each leaf with the bit set and clears the bit by an atomic and; `MmuFormat::ACCESSED`
- `kernel/arch/x86/lib/page_tables/test/page_tables_test.cpp` — three host tests of the walk
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp`, `kernel/arch/x86/mm/page_table.cpp` — `PageTable::harvest_accessed_pages()` under the table's lock
- `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp`, `kernel/arch/x86/mm/arch_vm_aspace.cpp` — `ArchVmAspace::harvest_accessed()` (declared before, never defined) and `invalidate_all()`
- `kernel/mm/include/ours/mm/types.hpp` — `AccessedVisitor`
- `kernel/mm/include/ours/mm/vm_aspace.hpp`, `kernel/mm/vm_aspace.cpp` — `VmAspace::harvest_accessed()`, `harvest_all_user_accessed()`
- `kernel/mm/include/ours/mm/page_queues.hpp`, `kernel/mm/page_queues.cpp` — `harvest_accessed()`, `take_accessed()`; the aging thread and `rotate()` harvest first
- `kernel/commands/page_queues_test.cpp` — `check_harvest()`, one check (12)
- `.agents/skills/ours-kernel-design/references/mm.md`, `.agents/tasks/page-reclaim/PLAN.md`, `docs/repairs/261030/mm.md` — item 2, the cost of the harvest

**Symptom**
None: step 3 of `.agents/tasks/page-reclaim/PLAN.md`. After step 2 a page the kernel read or wrote through the physmap stayed young, but a page a program used through its own mapping looked unused: the hardware sets the accessed bit in the entry, and nothing read it. Step 4 would have reclaimed a page in constant use, and the program would have asked the pager for it again.

**Cause**
Why it is shaped as it is:

- **The bit is set once per TLB fill, not per access.** The CPU sets the accessed bit when it loads the translation into its TLB, and a hit in the TLB walks nothing. So clearing the bit alone records nothing for the next period: a hot page keeps its cached translation, and its bit stays clear. The harvest must drop the translations it clears bits for (decision D2, 2026-10-30: the exact way, not Linux's). It does so once for the whole space (`invalidate_all()`, a CR3 reload on each CPU in `m_active_cpus`) and not page by page: a harvest clears bits all over the space, and naming them would be one `invlpg` each. The reload drops everything non-global, which is all of a user space's translations: its leaves are never global and no PCID is allocated (both asserted).
- **The clear is an atomic and.** The hardware writes the accessed and dirty bits of an entry without the table's lock. A load, clear and store could write back a dirty bit set between the load and the store, losing it. `std::atomic_ref<PteVal>::fetch_and(~ACCESSED)` cannot.
- **The visitor runs under the page table's spin lock, with the queue lock taken inside it.** That is what makes the descriptor safe to touch: a page leaves its object only after the object has unmapped it, and the unmap takes the page table's lock, so while the harvest holds it every page it finds mapped is still a page. The lock order is the VMO's lock, the page table's, the queue's; the harvest takes only the last two, and the aspace's mutex above them. Lockdep, on in dbg, reports nothing.
- **Each node's thread keeps its own pages' bits.** All the nodes' threads walk the same tables, so a leaf whose page is on another node is left alone (`take_accessed()` answers false and the bit stays), and that node's thread finds it. A 2 MiB leaf is a contiguous block, so its first page says whose it is, and all 512 pages are marked.
- **The walk goes over mappings of paged objects only.** A physical object's frames are not pages any queue knows. The shared zero page is passed over too: it is on no queue and the same frame is mapped everywhere.
- **The space-by-space walk takes a reference to each space.** `ALL_ASPACE_LIST_LOCK` is a spin lock, and a harvest takes the space's mutex, so the lock cannot be held across it. The walk holds a reference to the space it is on, which keeps it on the list (its destructor takes it off), and steps to the next under the lock with `try_adopt()`: a space on the list may have lost its last reference already and be waiting in its destructor for that lock.

**Change**
The walk, an ordinary range walk with the visitor as its action:

```cpp
auto harvest = [&visit, &count](Pte &entry, PageTableLevel const level, PhysAddr) {
    if ((entry.value & Format::ACCESSED) == 0 || !visit(address(entry.value), page_size(level))) {
        return;
    }
    std::atomic_ref<PteVal>(entry.value).fetch_and(~Format::ACCESSED, std::memory_order_relaxed);
    count += 1;
};
```

and the thread, before it rotates, which was only the rotation:

```cpp
auto _ = m_owner->harvest_accessed();
m_owner->rotate_reclaimable_queues(AgeReason::Timeout);
```

Harvested before, so that a page used in the period that ends is in the newest generation when that generation becomes one older.

**Reference**
Zircon `VmAspace::HarvestAllUserAccessedBits()` calling `X86ArchVmAspace::HarvestAccessed()`, whose leaf loop `HarvestMappingL0()` marks the page accessed (`PageQueues::MarkAccessed()`) and, for `TerminalAction::UpdateAgeAndHarvest`, rewrites the entry through `UpdateEntry()` with a `ConsistencyManager` that queues the TLB invalidation. Here the walk clears in place and the space is flushed once. Zircon skips a space not active since its last harvest (`AccessedSinceLastCheck()`); this does not yet (`docs/repairs/261030/mm.md` item 2).

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped); host tests 151 binaries, the page-table one with `HarvestAccessed.*` 3 tests ok.
- `pagequeues.test`: ok, 12 checks. The new one maps a supplied VMO's two pages into a user address space of the test's own, populated, reads the second through the mapping (a present page: no fault, no lookup, no software mark), and rotates twice: the ages are 2 and 1 (`ages after a read through the mapping: 2 and 1` in the log). `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`, `mempressure.test`, `vmaundo.test`, `usercopy.test`, `pmnode.test`, `pmzone.test`, `vmaseal.test`, `vmawx.test`, `vmaphys.test` ok.
- Controls, each restored after and rebuilt: without the shootdown, 2 and 2 on both of two runs (the read hits the TLB and sets no bit); with a harvest that takes no page, 2 and 2. Both fail that one check and no other.
- Real page tables: with `kernel.mm.page-aging-seconds=1` on the dbg image, the first harvest of a boot cleared 69 bits in the page tables of init and the shell, the later ones 0 once those had exited; no lockdep report, both images PASS.
- The `mm_numa = true` build as four nodes: a thread per node, `pagequeues.test` (ages 2 and 1), `pmnode.test`, `mempressure.test`, `usercopy.test` ok.
- clang-tidy: nothing on the lines changed; clang-format clean.
- Not measured: what the harvest costs. That is `docs/repairs/261030/mm.md` item 2.

**Deliberately not done**
- A test that puts the pages of one VMO on different nodes: the four-node build passes the same check, but its pages come from one node, so `take_accessed()` answering false for another node's page is not exercised.
- `ZapUnaccessed` (freeing page tables nothing used): no caller, `NotSupported`.
- Skipping a space that has not run since its last harvest, and flushing by page when few bits were cleared: both are performance candidates for item 2, to be measured.
- The kernel's own address space: its large leaves are the physmap's, every kernel access sets them, and clearing a global entry needs a shootdown (the rule of 2026-10-25).

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why does a harvest that only clears the accessed bits see nothing in the next period, and what does the shootdown change?
- Why is the visitor safe to touch a page's descriptor when the harvest holds no lock of the page's object?
- Why does each node's thread leave the bits of another node's pages set?

## Title: a reclaimer evicts the oldest pages a page source supplied, and a page written to is never one of them (2026-10-30)

**Files**
- `kernel/mm/include/ours/mm/page_queues.hpp`, `kernel/mm/page_queues.cpp` — `PageQueues::reclaim(target_frames)`, `oldest_reclaimable()` with the `Rc::try_adopt()` rule
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `VmCowPages::reclaim_page()` and `ReclaimResult`; `file_dirty_locked()`; `Cursor::writable_here()`
- `kernel/commands/page_reclaim_test.cpp`, `kernel/commands/BUILD.gn` — `pagereclaim.test`, new, 9 checks
- `.agents/tasks/page-reclaim/PLAN.md`, `.agents/skills/ours-kernel-design/references/mm.md`

**Symptom**
None: step 4 of `.agents/tasks/page-reclaim/PLAN.md`. The queues ranked pages by age and nothing read the ranking: a pager VMO's pages stayed in memory for ever, however old, however short memory was.

**Cause**
Why it is shaped as it is:

- **The reclaimer holds a raw pointer, and has to make it a reference without the owner's lock.** A page on a queue says who owns it by a raw `VmCowPages *`, and the owner may be dropping its last reference at that moment. `oldest_reclaimable()` calls `Rc::try_adopt()` on it under the queue lock, which answers empty for an owner that has lost its last reference. That is safe because the owner's destructor takes its pages off the queues, under that same lock, before its storage goes (step 1): while the reclaimer holds the lock, a destructor that is waiting on it has not freed anything. An owner that answers empty is passed over; its page leaves the queue when the destructor gets the lock.
- **Then everything is looked at again under the owner's lock.** Between choosing a page and taking the owner's lock, none is held, so the page may have moved, gone, or been written. `reclaim_page()` checks that the slot still holds that page and that the queues still file it as reclaimable, and only then unmaps it from every mapping (`change_range_locked(Unmap)`, as `take_pages_locked()` does), removes it from the list and frees it. The slot is a hole, so the next access raises a read request, which is how the page comes back.
- **The reference is dropped last.** `PageQueues::reclaim()` lets its `Candidate` go after both locks are released. It may be the last reference to the owner, and the destructor takes the queue lock and the tree's lock.
- **A page written is not clean, and nothing tracked that.** A page of a page source was handed out writable like any page the node owns, and written in place with nobody told: evicted, the write would have been lost. Two changes make "reclaimable" mean clean. A write that reaches `require_owned_page()` moves the page to the anonymous queue (`file_dirty_locked()`), where a reclaimer never looks. And a page of a node with a page source is returned `writable = false` by the read lookups (`writable_here()`), so a read fault maps it read-only even in a writable mapping, and the first write faults, comes back to `require_owned_page()` and is seen. Without the second, a program could write a page through a mapping it got by a read.

**Change**
The reclaimer's loop:

```cpp
auto looks = count(PageKind::Reclaimable);
while (freed < target_frames && looks != 0) {
    looks -= 1;
    Candidate candidate;
    if (!oldest_reclaimable(&candidate)) {
        break;
    }
    if (VmCowPages::ReclaimResult::Evicted ==
        candidate.owner->reclaim_page(candidate.index, candidate.page)) {
        freed += 1;
    }
}
```

and the dirty rule, at the one place a node's own page is handed out to be written:

```cpp
if (found.owner == m_target) {
    m_target->lock().assert_held();
    m_target->file_dirty_locked(found.page, index());
```

**Reference**
Zircon `VmCowPages::ReclaimPage()` with `EvictionAction`, called by the `Evictor` (`zircon/kernel/vm/evictor.cc`) on pages it takes from the oldest reclaim queue with `PageQueues::PeekIsolate()`, which returns the owner by the page's backlink as a reference taken under the page queues' lock; and the rule that a dirty page of a pager-backed VMO is on a queue of its own (`PageQueues::SetPagerBackedDirty()`, `MoveToPagerBackedDirty()`) that the evictor does not take from. Zircon tracks dirty by a page's state in the VMO and a write-protected mapping, and asks the pager for write permission; here the queue is the state, and nothing asks.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped); host tests 151.
- `pagereclaim.test`: ok, 9 checks, on the default and the four-node build; three runs of the default in a row. It checks: `reclaim(2)` evicts the two oldest of four pages (ages 1, 1, 0, 0) and no others; an evicted page is asked of the provider again, and one that stayed is not; supplied again, an evicted page reads as before; a written page leaves the reclaimable queues, and `reclaim(100)` evicts every page but it, which keeps what was written; a page is mapped read-only in a writable mapping; an evicted page is gone from the mapping (`query()` answers NotFound); and 400 VMOs made and dropped on one thread while another reclaims 3 pages at a time on every node (about 400 of the 1600 pages evicted in each run, none lost, every queue back where it was).
- Controls, together in one build, restored after: an eviction that does not unmap fails "taken out of the mapping"; a write that does not file the page dirty fails "a page written leaves the reclaimable queues" and "reclaim(100) ... keeps what was written". Exactly those three of nine.
- `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`, `mempressure.test`, `vmaundo.test`, `usercopy.test`, `pmnode.test`, `pmzone.test`, `vmaseal.test`, `vmawx.test`, `vmaphys.test`, `pagequeues.test` ok.
- clang-tidy: nothing on the lines changed; clang-format clean.
- Not verified: the lifetime rule against a drop at the exact moment between `try_adopt()` and the owner's lock. The stress makes it likely over many rounds, not certain; there is no control that removes `try_adopt()`, since the alternative is a use after free.

**Deliberately not done**
- Nothing calls `reclaim()` but the test: the thread and the watchdog's wiring are step 6 (the user chose a synchronous call for this step).
- No age floor: `reclaim()` takes the oldest pages whatever their age, until it has its target or runs out. How much, and from which node, is the caller's to say in step 6.
- Writing a dirty page back to the source: a page written stays in memory for good (non-goal of the design). A user-space file system that writes through pager VMOs sees its pages pile up on the anonymous queue.
- Anonymous pages: step 5.

**How it was found**
The dirty rule was found while reading `require_owned_page()` to see what a write to a pager page does before building the eviction: it returned the page writable with no record, and a read fault mapped it writable too.

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why is `try_adopt()` called under the queue lock, and what in the VMO's destructor makes that sufficient?
- Why does `reclaim_page()` check the page again after taking the owner's lock, when the reclaimer has just chosen it?
- Why must a page of a page source be mapped read-only even in a writable mapping, for eviction to be safe?

## Title: an anonymous page that is all zero becomes a zero marker and is freed (2026-10-30)

**Files**
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `VmCowPages::dedup_zero_page()`, `is_zero_filled()`
- `kernel/mm/include/ours/mm/page_queues.hpp`, `kernel/mm/page_queues.cpp` — `PageQueues::dedup_zero_pages(target_frames)`, `oldest_zero_fork()`; `adopt_first_locked()` shared with the ring's `oldest_reclaimable()`
- `kernel/commands/page_reclaim_test.cpp` — six checks (15), among them a writer racing the dedup
- `.agents/tasks/page-reclaim/PLAN.md`, `.agents/skills/ours-kernel-design/references/mm.md`

**Symptom**
None: step 5 of `.agents/tasks/page-reclaim/PLAN.md`. A process that touched memory and left it zero (a zeroed buffer, a freed and cleared heap block) kept a physical page for each, for as long as it lived.

**Cause**
Why it is shaped as it is:

- **Only the zero-fork queue is looked at.** Step 1 filed the pages committed in place of zero content (a write to a hole or a zero marker) apart from the others, because they are the ones likely to be zero again. A page found not to be zero leaves that queue for the anonymous one, so each page is looked at once and a page full of data is never scanned again.
- **Two looks, and the second is the one that counts.** The first look reads the page while a program may be writing it, so it may see a page half written, and it costs no TLB shootdown. It is only worth taking before the expensive step: a page that is plainly not zero must not lose Write in every mapping, since every program writing it would then fault. A page that looks zero loses Write (`change_range_locked(RemoveWrite)`, which returns only once no CPU holds a writable entry for it), and is looked at again. Now it cannot change: a write to it faults, and the fault needs the lock this holds. Only a page that is zero the second time goes on.
- **Unmapped before it is freed, and the slot keeps the content.** The entries are taken out of every mapping (`Unmap`) and the page is swapped for a zero marker in place, which allocates nothing and cannot fail (`VmPageList::replace_page_with_zero_marker()`), then freed. A read of the marker is the shared zero page, read-only; a write commits a new zeroed page.
- **Leaf nodes only.** A hidden node's pages are seen by every node below it, through mappings that are not the hidden node's to take Write from (`change_range_locked()` reaches the objects over a node, and a hidden node has none). So such a page, and a page in a node that has children, is left and filed anonymous. The user decided this on 2026-10-30 (the open question of `DESIGN.md`): most anonymous zero pages are in a leaf.

**Change**
The core of `dedup_zero_page()` under the node's lock:

```cpp
if (!is_zero_filled(page)) {
    return leave();
}
change_range_locked(offset, PAGE_SIZE, RangeChange::RemoveWrite);
if (!is_zero_filled(page)) {
    return leave();
}
change_range_locked(offset, PAGE_SIZE, RangeChange::Unmap);
auto *const taken = m_pages.replace_page_with_zero_marker(index);
free_page(page);
```

where `leave()` files the page anonymous and answers Kept. The candidate and its owner come the same way as for a pager page (`try_adopt()` under the queue lock, dropped with no lock held).

**Reference**
Zircon `VmCowPages::DedupZeroPage()` (`zircon/kernel/vm/vm_cow_pages.cc:1363`), which the scanner (`scanner.cc:296`) calls on the pages `PageQueues::PopAnonymousZeroFork()` returns. Its order is the same, and it gives the same reason for the first look: a racy check that leaves Write on the page, since most pages are not zero, then `RangeChangeUpdateLocked(... RemoveWrite)`, a check again, then the slot is cleared under an `Unmap`. Zircon also refuses a high-priority address space, and dedups a hidden node's pages when it can; this does neither.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped); host tests 151.
- `pagereclaim.test`: ok, 15 checks, on the default and the four-node build, and in a session of 15 tests run twice in a row with `dedup_zero_pages(~0)` taking every zero page on the node, other processes' included (all ok afterwards). The new ones: pages written with zeros become markers and are freed (committed pages 4 to 2), every byte reads as before, a page with something in it is left and filed anonymous and a second pass leaves it, a write to a marker commits a new page; a zero page is gone from the mapping that showed it (`query()` answers NotFound); and a writer thread attached to a user address space writes a value, reads it back and writes zero again, 20000 times over 8 pages, while the main thread deduplicates: 0 lost, 68 to 1527 zero pages freed meanwhile across runs.
- Controls, each restored after: without the `Unmap`, "a zero page is taken out of the mapping" fails every time. Without the `RemoveWrite`, the race check lost one write in one of three runs (1 of 20000 writes), and none in the other two: the window between the second look and the unmap is a few microseconds, so this check is a probabilistic one, and a run that passes proves little. The correctness of the second look rests on the argument above, not on this test.
- clang-tidy: nothing on the lines changed; clang-format clean.

**Deliberately not done**
- Hidden nodes' zero pages (see above): kept for the shared-page case, recorded as an open question in `DESIGN.md`.
- Nothing calls `dedup_zero_pages()` but the test: step 6.
- The first look reads a page a program may be writing, with plain loads. The result is only a filter and the second look is authoritative; it is not an atomic read.
- A page deduplicated and written again goes through a zero marker and a new allocation each time: a page that flips between zero and not, as the race test does, costs an allocation and a shootdown per flip. Whether that matters is for a measurement.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why is the page looked at again after Write is taken away, and why is the second look enough?
- Why is the first look taken before taking Write, when it can be wrong?
- Why is a hidden node's zero page left alone?

## Title: the memory watchdog has the reclaimer give memory back at Warning, Critical and OutOfMemory (2026-10-30)

**Files**
- `kernel/mm/include/ours/mm/memory_reclaimer.hpp`, `kernel/mm/memory_reclaimer.cpp`, `kernel/mm/BUILD.gn` — `MemoryReclaimer`: a thread, `request()`, `reclaim()`, `Pause`; started by an init hook after the watchdog's level
- `kernel/mm/memory_watchdog.cpp` — `look()` asks it
- `kernel/commands/page_reclaim_test.cpp` — three checks (18), among them the whole path from the watchdog's levels to freed pages
- `kernel/commands/memory_pressure_test.cpp` — its watchdog checks run with the reclaimer paused
- `.agents/tasks/page-reclaim/PLAN.md`, `.agents/skills/ours-kernel-design/references/mm.md`, `docs/repairs/261030/mm.md` — items 3 and 4

**Symptom**
None: step 6 of `.agents/tasks/page-reclaim/PLAN.md`, the last. The watchdog could only refuse: at OutOfMemory the allocations that could wait failed, because nothing was given back, though steps 4 and 5 had made two kinds of page that could be.

**Cause**
Why it is shaped as it is:

- **Two ways in, by what the watchdog can afford to wait for.** At Warning and Critical memory is short but not out, so `look()` calls `request()`, which records what is wanted and wakes the reclaimer's own thread, and goes on. At OutOfMemory the allocations that could wait are about to be refused, so `look()` calls `reclaim()` itself and waits: what is found afterwards is what is declared and what the PMM's gate is armed for. Free memory that was brought back above the OutOfMemory level is never declared OutOfMemory, and nothing is refused. That is D3 of the task: only when the watchdog asks, and once synchronously at OutOfMemory.
- **The ask is a distance, not a flag.** The target is the upper edge of the bounds of the pressure found (`bounds_of(found).upper`), minus the free frames: how far free memory has to rise for the watchdog to see the pressure above. A request made while another is being served is the larger of the two, not the sum, since the watchdog asks again at each look with the same distance.
- **Zero pages before pager pages, on every node.** `reclaim()` first asks every node's `dedup_zero_pages()`, then every node's `reclaim()`, each for what is left of the target. A zero page costs nothing later; a pager page is asked of its source again (the user's choice, 2026-10-30: option a).
- **Nothing can wait for the thread at OutOfMemory, and nothing the thread takes waits for the watchdog.** The allocations that wait do so with no VMO lock held (`AnonymousPageRequest`), so the reclaimer, which takes VMO locks, cannot be waited for by a holder of one; the watchdog holds only its own mutex while it reclaims.
- **A pause for the tests of the decision.** `mempressure.test` moves the levels to where free memory is and looks at what the watchdog declares; with pages to give, what the reclaimer gives back would move free memory off the levels. `MemoryReclaimer::Pause` turns it off for those checks, as `MemoryWatchdog::Hold` does for the gate.

**Change**
`look()`, before and after:

```cpp
// before
auto const free = approx_free_frame_count();
auto const found = m_levels.pressure_at(free);

// after
auto free = approx_free_frame_count();
auto found = m_levels.pressure_at(free);
if (found == MemoryPressure::OutOfMemory) {
    auto const goal = m_levels.bounds_of(found).upper;
    if (auto const freed = MemoryReclaimer::get().reclaim(goal - free); freed != 0) {
        free = approx_free_frame_count();
        found = m_levels.pressure_at(free);
    }
}
if (found == MemoryPressure::Warning || found == MemoryPressure::Critical) {
    MemoryReclaimer::get().request(m_levels.bounds_of(found).upper - free);
}
```

**Reference**
Zircon's `Evictor` keeps an eviction target that its callers add to (`Evictor::CombineEvictionTarget()`, `Evictor::EvictionTarget`) and a scanner thread serves (`zircon/kernel/vm/scanner.cc`), the same split as `request()` and the thread here. I did not trace what raises its target when memory runs short, so whether Zircon also reclaims on the thread that declares the worst pressure is not known. Linux `kswapd` and direct reclaim are not followed: nothing here reclaims in the background, by D3.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped); host tests 151.
- `pagereclaim.test`: ok, 18 checks, on the default and the four-node build. At Critical (levels moved so that free memory is 8 frames under Critical, with 64 zero pages and 64 pager pages made): the watchdog asked for 16 frames and 16 were given back, all of them zero pages (48 of 64 left, 64 of 64 pager pages left). With about 208 frames asked and 112 to give, every zero page and every pager page went. At OutOfMemory with pages to give: `out of memory: gave back 48 frames before refusing anything`, the pressure declared is not OutOfMemory, and an allocation that could wait is served.
- `mempressure.test`: ok with the reclaimer paused for its watchdog checks, including the refusal at OutOfMemory when there is nothing to give; run before and after `pagereclaim.test` in one session. The other 13 mm tests ok; no lockdep report.
- Controls, each restored after: without the reclaim at OutOfMemory, the OutOfMemory check fails (1 of 18); without the request at Warning and Critical, both Critical checks fail (2 of 18) and the OutOfMemory one still passes.
- clang-tidy: nothing on the lines changed; clang-format clean.

**Deliberately not done**
- The reclaimer asks once at each look, and the watchdog looks again only when free memory leaves its bounds or the hysteresis ends. If a request cannot be met, nothing asks again until the pressure changes: reclaim when asked, not continuously (D3).
- No age floor and no choice of node: it gives back the oldest first on every node in turn, up to the target.
- A refetch costs more than a swap-in might, and the reclaimer and a VMO's users contend: `docs/repairs/261030/mm.md` item 3, to measure.
- A pager that cannot drop pages it supplied, and written pager pages that stay: `docs/repairs/261030/mm.md` item 4.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why does the watchdog reclaim on its own thread at OutOfMemory, and on the reclaimer's at Critical?
- Why is the target a distance to the upper edge of the bounds and not a fixed number of frames?
- Why does `mempressure.test` pause the reclaimer for its watchdog checks?
