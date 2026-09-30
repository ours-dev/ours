# mm changes

## Title: ZERO_PAGE is allocated zeroed (2026-09-29)

**Files**
- `kernel/mm/init_vmm.cpp` — `init_vmm_preheap()` allocates the zero page with `GAF_KERNEL | Gaf::Zero` and checks in the debug image that it reads as zero.

**Symptom**
Latent, cannot be triggered today: nothing reads `ZERO_PAGE` or `get_zero_page()`, so no one has seen it. It would be triggered by the first reader, for example a copy-on-write child answering a read of a hole with the zero page, which would return whatever the previous owner of the frame left in it.

**Cause**
`Gaf::Kernel` is `ThisNode | Reclaim | Write` (`kernel/mm/include/ours/mm/gaf.hpp`); only `Gaf::Boot` and `Gaf::User` carry `Zero`. The page allocator honours `Zero` in `PmZone::allocate()` (a fill of the whole block after the buddy call), and the buddy allocator gives a frame back exactly as it was freed. So a frame taken with `GAF_KERNEL` alone is not zero, and `init_vmm_preheap()` took it that way and then pinned it as the shared zero page.

It went unnoticed for two reasons that compound. Nothing reads the page, so no behaviour depends on it. And a fresh QEMU guest starts with zeroed RAM, so even a reader would usually see zeros at the first boot and fail only after frames have been freed and reused, which is the kind of failure that appears far from its cause. `VmCowPages::alloc_pages()` already carries a comment about exactly this trap ("`GAF_KERNEL` does not imply it"); this call site was the one that predates it.

**Change**
Before:

```cpp
auto frame = alloc_frames(GAF_KERNEL, usize(0));
```

After:

```cpp
auto frame = alloc_frames(GAF_KERNEL | Gaf::Zero, usize(0));
...
DEBUG_ASSERT(std::all_of(frame_to_virt<isize>(ZERO_PAGE),
                         frame_to_virt<isize>(ZERO_PAGE) + PAGE_SIZE / sizeof(isize),
                         [](isize const word) { return word == 0; }),
             "the zero page is not zero");
```

The assertion is a `DEBUG_ASSERT`, so the release image pays nothing, and it runs once on one page. It is there because a booted kernel is the only evidence a change here can give and nothing else looks at the page.

Rejected: zeroing by hand with `std::fill_n` after the allocation, as Zircon does. `Gaf::Zero` does the same fill in the allocator, and a second spelling of the same fact would drift from it.

**Reference**
Zircon `vm_init_preheap()` (`zircon/kernel/vm/vm.cc:181-190`): `pmm_alloc_page()` for the zero page, `set_state(WIRED)`, then `arch_zero_page()` on its physmap address.

**Verification**
- `scripts/format_cpp.py --check kernel/mm/init_vmm.cpp`: clean. `clang-tidy -p out/vmo-snapshot kernel/mm/init_vmm.cpp`: no diagnostic on the changed lines; the ones reported are in headers this change does not touch.
- `boot_check.py --out out/vmo-snapshot`: both images PASS. dbg: 18 passed, 0 failed, 0 error lines. rel: 13 passed, 9 skipped, 0 failed, 0 error lines.
- The assertion string is present in the `kernel_x86.dbg.1` object (the debug image) and absent from `kernel_x86.dbg.0` (the release one), so the dbg boot did run the check.
- Not shown: the bug itself. A fresh guest has zeroed RAM, so the page was probably zero before this change as well; the boot cannot tell the old code from the new. The change rests on the allocator contract, not on an observed failure.

**Deliberately not done**
- No reader of `ZERO_PAGE` was added. That is the copy-on-write child's cursor, a later step of `.agents/tasks/vmo-snapshot-child/`.
- The `VmmBody` fields of the page (`vmo`, `vmo_index`, `num_mappings`) were not looked at. A page mapped by many address spaces does not fit "one page, one owning VMO", which matters when the first reader maps it.
- The stale comment in `kernel/object/vmo_dispatcher.cpp` ("there is no shared zero page yet") was left; it is a different file and a different subject, and it becomes true or false depending on what the copy-on-write work decides.

**How it was found**
Read while reviewing the design of the copy-on-write child VMO: the design needed "a page that reads as zero", and `grep -rn ZERO_PAGE kernel` showed the definition in `vmm.cpp`, the allocation in `init_vmm.cpp`, the accessor in `vmm.hpp` and no other use. Checking how the frame is allocated against the `Gaf` values then showed the missing `Zero`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `GAF_KERNEL` not imply `Gaf::Zero`, and which two `Gaf` presets do?
- Why could a non-zero `ZERO_PAGE` stay invisible through many boots, and what would the first symptom look like?
- Why is the check a `DEBUG_ASSERT` rather than an `ASSERT`, and what does a passing dbg boot prove and not prove about this fix?

## Title: Cursor lookups return the page together with whether it may be written (2026-09-29)

**Files**
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — new `Cursor::RequireResult{page, writable}`; `require_page()`, `require_read_page()` and `require_owned_page()` return `ktl::Result<RequireResult>`.
- `kernel/mm/vm_cow_pages.cpp` — the three return sites answer `writable = true`.
- `kernel/mm/vm_mapping.cpp` — `map_paged()` and `page_fault()` take `.page`; `map_paged()` asserts `writable`, and `page_fault()` carries a comment saying the bit must be honoured once it can be false.
- `kernel/mm/vm_object_paged.cpp` — `copy_page_by_page()` takes `.page` and asserts that a write got a writable page.

**Symptom**
Latent, cannot be triggered today: every page an object hands out is its own, so every answer is writable. It becomes a data corruption the day a lookup can answer with a page on loan from an ancestor: the callers have no way to tell, and `page_fault()` maps every page with the mapping's own permissions, so a write to a borrowed page would go straight into the lender's page.

**Cause**
`Cursor::require_*()` returned a bare `VmPage *`. Whether a page may be written is not a property of the page: the same physical page is the writer's own in the object that owns it and content on loan in an object whose ancestor owns it. Only the cursor knows which object it found the page in, so a caller that is given only the pointer cannot recover the answer, and the interface has to carry it.

There is a second reason it belongs in the return value rather than in a later fix inside the fault path. The spurious-fault branch of `page_fault()` repairs a present page-table entry whose permissions differ from the mapping's by calling `protect()` in place. A borrowed page mapped read-only under a writable mapping is exactly such an entry, so without the bit a first write would be "repaired" into a writable mapping of the lender's page. The bit is what lets the fault path tell that entry from a stale one.

**Change**
Before:

```cpp
auto require_owned_page(usize nr_pages, MultiPageRequest *request) -> ktl::Result<VmPage *>;
```

After:

```cpp
struct RequireResult {
    VmPage *page;
    bool writable;
};
auto require_owned_page(usize nr_pages, MultiPageRequest *request) -> ktl::Result<RequireResult>;
```

No behaviour change: the three return sites are the object's own store and a fresh allocation, both writable. The callers only take `.page`. Two `DEBUG_ASSERT`s state the invariant that holds today: `map_paged()` asks for owned pages, and a write in `copy_page_by_page()` asks for one; both must come back writable. `page_fault()` does not consult the bit yet; its comment says why it will have to.

Rejected: adding a tag-aware lookup to `VmPageMap` in the same step. Nothing inserts a `Borrowed` slot, and a child's page store will hold only its own pages, so it would have no caller.

**Reference**
Zircon `LookupCursor::RequireResult{page, writable}` (`zircon/kernel/vm/include/vm/vm_cow_pages.h`), filled by `PageAsResultNoIncrement()` in `vm_cow_pages.cc`; Linux's counterpart is the write-protect fault of a private file mapping, `do_wp_page()` in `mm/memory.c`, which exists because a page-cache page is mapped read-only for a writable VMA.

**Verification**
- `scripts/format_cpp.py --check` on the four files: clean. `clang-tidy -p out/vmo-snapshot` on the three sources: nothing on the changed lines; the warnings reported (private member names in `MappingCoalescer`, `NULL` in three places, an `override` on the destructor) are older code this change does not touch.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts as before the change. dbg: 18 passed, 0 failed, 0 error lines. rel: 13 passed, 9 skipped, 0 failed, 0 error lines.
- Not measured: anything about a non-writable answer. None exists yet, so the new branch of the interface is not exercised.

**Deliberately not done**
- No non-writable answer, no parent in `VmCowPages`. This step only makes room for one.
- `page_fault()` and `map_paged()` do not yet map by `writable`; that is the step that gives `writable` a `false` to honour, and it has to change both.

**How it was found**
While designing a copy-on-write child VMO: walking a write to a `.data` page through `page_fault()` showed that the fault path had no way to be told the page it maps is on loan.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is "may this page be written" a property of the lookup, not of the page?
- Which branch of `page_fault()` would turn a borrowed read-only entry into a writable one, and why is that branch correct today?
- Why does `map_paged()` assert `writable` instead of mapping by it?

## Title: VmCowPages takes its lock from a shared HierarchyState (2026-09-29)

**Files**
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — new private `HierarchyState`, a reference-counted holder of one `sync::Mutex`; `VmCowPages` keeps an `Arc<HierarchyState>`, and `m_lock` becomes a reference to that mutex. The constructor takes the hierarchy as a parameter.
- `kernel/mm/vm_cow_pages.cpp` — `create()` makes a `HierarchyState` for the new node and hands it to the constructor.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — the order of work: this step is only the lock, and the parent link moves to the next one.

**Symptom**
None. Latent, cannot be triggered today: no `VmCowPages` has a parent, so every tree has exactly one node, and the lock behaves as the member it replaces. It matters once a node can hang under another.

**Cause**
The mutex was a member of `VmCowPages`, so it lived and died with that one node. A copy-on-write tree has many nodes that must be locked by one mutex (a read walks from a leaf up through its ancestors, and a clone changes several nodes at once), and any node can die before the others: the launcher closes its handle to the file VMO while the clones of it run. A mutex that is a member of the node that dies is then gone while the survivors still hold references to it, and a lock that is not shared is not a lock over the tree. So the mutex has to be an object of its own, kept alive by a reference from every node.

**Change**
Before:

```cpp
VmCowPages(Gaf gaf, usize size);
...
sync::Mutex m_lock;
```

After:

```cpp
class HierarchyState final: public ustl::RefCounted<HierarchyState, ustl::sync::ThreadSafePolicy> {
    ...
    sync::Mutex m_lock;
};
VmCowPages(Gaf gaf, usize size, ustl::Arc<HierarchyState> hierarchy);
...
ustl::Arc<HierarchyState> m_hierarchy;
sync::Mutex &m_lock; // the hierarchy's
```

`lock()` still answers a `sync::Mutex &`, and `VmObject::m_lock` is still a reference to it, so no `Guard` and no `TA_REQUIRES` had to change. `m_lock` stays a member name (now a reference) so the thread-safety annotations still name a member, as `VmObject::m_lock` already does. The hierarchy is a constructor parameter and not made inside it, because making one can fail and a constructor cannot say so; `create()` makes one and returns `OutOfMem` if it cannot.

Rejected: one lock per node with a lock-order number, as Zircon does now. That is decision 3 of the design and the user chose one mutex per tree for now; the benchmark after the fault path works decides whether it is worth the lock-order argument.

**Reference**
Zircon `VmHierarchyState`, held by every `VmCowPages` as `hierarchy_state_ptr_` and returned by `lock()`, until commit `2dad682fe14` ("Cleanup old locking") replaced it with one lock per node. In this tree `PeerHolder` (`kernel/object/include/ours/object/dispatcher.hpp`) has the same shape for the two ends of a channel.

**Verification**
- `scripts/format_cpp.py --check`: clean. `clang-tidy -p out/vmo-snapshot kernel/mm/vm_cow_pages.cpp`: nothing on the changed lines; the one warning is the existing destructor without `override`.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts as before. dbg: 18 passed, 0 failed, 0 error lines. rel: 13 passed, 9 skipped, 0 failed, 0 error lines.
- What the boot does exercise: every VMO the system makes goes through `create()` and takes and releases this lock, so the plumbing is used by the whole boot, kernel stacks and BootFS included.
- Not exercised: sharing one hierarchy between two nodes, and a node dying while another still holds the hierarchy. No node can hang under another yet; the next step's test builds a chain by hand and checks both.

**Deliberately not done**
- No `m_parent`, offset, limit or hidden flag yet. They would have no user in this commit and a boot cannot test them; they arrive with the walk that reads them and the test that builds a chain.
- No list of children: the first code that walks it is the clone and the merge.
- The mutex was not given a name for lock tracing; it has none today either.

**How it was found**
Designing where the lock lives once a clone exists: the design asked what happens when the node that made the lock is the first to die, which is the launcher's case (it closes the file handle and the clones live on).

**The user's prediction**
None. The user chose one mutex per tree (decision 3) without writing the invariant argument, and said to go on; the argument is still owed and matters most from the step that gives a node a parent.

**Three questions you should be able to answer**
- Why can the tree's mutex not be a member of the first node?
- What does a node hold to keep the mutex alive, and what would go wrong if it held a plain reference?
- Why does `VmObject::m_lock` need no change, given the mutex moved?

## Title: A VmCowPages can hang under another, and a read walks up to find its page (2026-09-29)

**Files**
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — `m_parent`, `m_parent_offset`, `m_parent_limit`, `m_hidden`; `VmCowPages::create_child()`, `is_hidden()`, `nr_pages_locked()`; `Cursor::find_ancestor_page()`.
- `kernel/mm/vm_cow_pages.cpp` — the constructor takes the parent link; `create_child()` clamps the limit and adopts the parent's `HierarchyState`; `find_ancestor_page()` is the walk; `require_read_page()` answers from above for a node with a parent or a hidden one; `require_owned_page()` refuses a hidden node and, for now, a node with a parent that has no page of its own.
- `kernel/mm/include/ours/mm/vm_page_map.hpp` — `size()`, which only the test uses.
- `kernel/mm/vm_mapping.cpp` — `page_fault()` asserts that the page it maps is writable.
- `kernel/commands/vmo_snapshot_test.cpp`, `kernel/commands/BUILD.gn` — the kernel command `vmosnapshot.test`, 35 checks.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — the order of work marks this step done.

**Symptom**
None. Latent, cannot be triggered today: `create_child()` has no caller outside the test, so no node the rest of the kernel sees has a parent. It is the first half of the read path of a copy-on-write child.

**Cause**
`VmCowPages` had no parent, so its store could only answer "this node has the page" or "nobody has it", and a read of a hole was answered by committing a zeroed page. A child that shares its parent's unwritten pages needs a third answer, "an ancestor has it, and it is not yours to write", and it needs to say that without allocating anything in the child: a read that allocated would copy on every read and defeat the point.

Two rules decide the shape of the walk, and both come from where the coordinates live. The parent offset and the limit belong to the node whose parent it is, so an offset is compared with that node's limit and only then moved into the parent's coordinates; comparing after the move, or with the parent's limit, reads past the end of what the child is allowed to see. And the walk never looks at the target's own store, because the callers have looked there already; looking again would hide the difference between "the node has it" (writable) and "an ancestor has it" (on loan).

**Change**
The walk:

```cpp
while (node->m_parent && offset < node->m_parent_limit) {
    offset += node->m_parent_offset;
    node = node->m_parent.as_ptr();
    if (auto *const page = node->m_pages.get_page(PgOff(offset >> PAGE_SHIFT))) {
        return page;
    }
}
return nullptr;
```

A read of a node with a parent, or a hidden one, answers the ancestor's page with `writable = false`, or `ZERO_PAGE` with `writable = false` when the walk finds none, which includes an offset past a limit. A node with no relatives still commits a zeroed page of its own on a read, as before, although `ZERO_PAGE` and the writable bit now exist: the fault path does not yet map by `writable`, and handing the shared page to a caller that maps every page as writable would let a write through. `page_fault()` now asserts `page->writable`, so that day is loud, in the debug image, and not silent.

A hidden node refuses a writer (`BadState`) whether or not the slot is full, and reports its own pages as not writable. A write to a node with a parent and no page of its own answers `Unimplemented`, and does not allocate a zeroed page, because that would discard what the parent holds; the copy is the next step.

`create_child()` clamps the limit to the child's size and to what the parent has past the offset (Zircon `ClampedLimit()`), so a caller may pass a length it has not checked, and refuses an argument that is not page aligned.

**Reference**
Zircon `VmCowPages::FindPageContentLocked()` (`vm_cow_pages.cc:2958`), which loops `while (offset < cur.parent_limit_)` and adds `parent_offset_` at each hop; `LookupCursor::RequireReadPage()`, which returns an ancestor's page with `writable = false` and `vm_get_zero_page()` past the limit; `ClampedLimit()` (`:113`); `VmCowPages::is_hidden()`.

**Verification**
- `scripts/format_cpp.py --check`: clean. `clang-tidy -p out/vmo-snapshot` on `vm_cow_pages.cpp` and the test: nothing on the changed lines; the one warning is the existing destructor without `override`.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts as before. dbg: 18 passed, 0 failed, 0 error lines. rel: 13 passed, 9 skipped, 0 failed, 0 error lines.
- `boot_check.py` does not run kernel shell commands, so `vmosnapshot.test` was driven over COM1 with a script that waits for `--- end ---` and types the command: 35 checks ok on both images. The older commands still pass: `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 and `vmaseal.test` 13 on rel.
- The test can fail. With the line `offset += node->m_parent_offset;` commented out, the debug image failed exactly four checks (the shifted child's page 0 and page 1, the clamped child, and the chain through the shifted child), 4 of 35; the line was restored and all 35 pass again.
- The test also checks `ZERO_PAGE` for all zeros, which is the first check of the fix in the earlier entry of this file.
- Not exercised: a node's own page winning over an ancestor's (a node with a parent cannot get a page of its own until a write copies one), any write, and concurrency. The test is single-threaded.

**Deliberately not done**
- No write. A write to a node with a parent answers `Unimplemented`; the fork-or-migrate write and the share count are the next step.
- The parent does not know its children. Nothing walks down yet; the list arrives with the clone and the merge.
- The fault path does not map by `writable`. No node with a parent is behind a mapping, and the assertion says so.
- No recursion guard on destruction: a node holds its parent, so dropping the last reference to a long chain releases it recursively. Chains are one or two deep here; Zircon walks up iteratively.

**How it was found**
None: a new mechanism, taken from Zircon's lookup and adjusted to this tree's store. The offsets' coordinates were checked by hand against the four cases the test builds (a shifted child, a clamped one, a chain, a chain through a shifted node), and the mutation above showed the test sees them.

**The user's prediction**
None.

**Three questions you should be able to answer**
- In `find_ancestor_page()`, why is the offset compared with the node's limit before the parent offset is added, and what does a page-3 read of the grandchild do at the middle node?
- Why does a node with no relatives still commit a zeroed page on a read, when `ZERO_PAGE` exists?
- Why is a write to a node with a parent refused, rather than answered with a zeroed page like a hole of a plain node?

## Title: A VmCowPages keeps a list of the nodes hanging under it (2026-09-29)

**Files**
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — the sibling hook, `ChildList` and `m_children`, `nr_children_locked()`; `m_parent_offset` and `m_parent_limit` are no longer `const`.
- `kernel/mm/vm_cow_pages.cpp` — `create_child()` links the new node onto its parent's list, and the destructor takes the node off it.
- `kernel/commands/vmo_snapshot_test.cpp` — 9 more checks, 44 in all.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — the order of work, and a correction (below).

**Symptom**
None. Latent, cannot be triggered today: nothing reads the list. It has to be right from the first node, because a clone that replaces a node in its parent's list, and a merge that looks for the sibling that is left, can only trust a list that was never wrong.

**Cause**
A parent could not find its children. The parent's list is a bare link in each child, not a reference, because a child already holds its parent and a list that held the children would make a cycle that nothing could break. A bare link means the child has to take itself off before it is freed, or the parent's list points at freed memory; and taking itself off means taking the tree's lock, because a walk of the list is only safe against a child that is either wholly on it or wholly off. That is why the destructor now takes the lock, and why this needed its own step rather than being a line in the clone.

**Change**
```cpp
VmCowPages::~VmCowPages() {
    if (m_parent) {
        Guard guard{m_lock};
        m_parent->lock().assert_held();
        m_parent->m_children.erase(*this);
    }
    m_pages.drain_owned(...);
}
```
`create_child()` adopts the node into an `Arc` first and then pushes it onto the parent's list under the lock, so that the destructor that would unlink it is the one that runs whatever happens between the two lines. The hook is a safe link: its destructor traps on a node still linked, so a node that forgot to leave cannot be freed quietly. The mutation check below relied on that.

`m_parent_offset` and `m_parent_limit` lose their `const`: a clone puts a new node above an existing one and rewrites its parent link. They are read by cursors, which are made under the lock, and written only under it.

**Correction to the design**
DESIGN.md said a clone rewrites each page's owner back-pointer through the page queues, as Zircon does. `set_queue_backlink_locked()` is called by nothing outside `page_queues.cpp`, and no VMO page is ever put on a queue or given a back-pointer, so there is nothing to rewrite. The design now says so, and keeps the note for the day pages are queued for reclamation.

**Reference**
Zircon `VmCowPages::children_list_` and `AddChildLocked()` / `RemoveChildLocked()` (`vm_cow_pages.cc:1488`, `:2030`), where a child is added to its parent's list under the parent's lock and removed from it when it dies. Zircon's `~VmCowPages` asserts the node is off every list; here the destructor does the removal itself.

**Verification**
- `scripts/format_cpp.py --check`: clean. `clang-tidy -p out/vmo-snapshot`: nothing on the changed lines; the one warning is the existing destructor without `override`.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts as before. dbg: 18 passed, 0 failed, 0 error lines. rel: 13 passed, 9 skipped, 0 failed, 0 error lines.
- `vmosnapshot.test` over COM1: 44 checks ok on both images (35 before, 9 new: three children listed, none listed for a childless node, a dying child leaves the list, a grandchild is on its own parent's list and not its grandparent's, a child stays alive and listed when its parent handle is dropped, and leaves with its parent when it dies, and the parent ends with none). `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel, still pass.
- With the `erase` line removed, the debug image took an invalid-opcode trap in the test, from the safe hook's destructor, so a node that fails to leave is loud. The line was restored and all 44 pass again.
- Not exercised: concurrency. The test is single-threaded, and nothing reads the list yet.

**Deliberately not done**
- Nothing reads the list. The clone (replacing a node in its parent's list) and the merge (finding the sibling that is left) are the first that will.
- No protection against resurrecting a child that is being destroyed. It cannot happen while nothing takes a reference from the list; the merge will need a dead transition and a try-acquire, as Zircon's does. Written up under the open questions of the design.
- The order of the list is creation order and nothing depends on it.

**How it was found**
None: a step of the design. The open question about walking the list against a dying child came out of writing the destructor, since it is exactly the window in which the node is still on the list and its count is already zero.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the parent's link to its children not a reference, and what would happen if it were?
- Why does the destructor need the tree's lock, given that nobody else can hold a reference to the node?
- What can go wrong for a reader that takes a reference from the list while a child is being destroyed, and why is it not a problem yet?

## Title: A VmCowPages can be cloned by putting a hidden node above it (2026-09-30)

**Files**
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `create_clone_locked()`, `parent_locked()`, `find_ancestor_page()` (the walk, moved from the cursor onto the node), `make_node()` (the one allocation of a node, now used by `create()`, `create_child()` and the clone). The destructor skips a node that was never linked.
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `VmmBody::share_count`, `PageFrame::share_count()` and `add_share()`.
- `kernel/mm/include/ours/mm/vm_page_map.hpp` — `has_page_in()` and `take_all_from()`.
- `kernel/commands/vmo_snapshot_test.cpp` — 30 more checks, 74 in all.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — this step done; pinning is refused by the `VmObject` layer.

**Symptom**
None. Latent, cannot be triggered today: `create_clone_locked()` has no caller outside the test. It is the step that makes a copy-on-write child possible: a node that shares its source's pages without copying them, and that neither the source's nor its own later writes can reach.

**Cause**
A child that only borrows its parent's pages is correct while the parent never changes, and wrong the moment it does: the child would see the parent's later writes. The way out is to make no node that others share ever writable. The pages every side sees are put in a node above all of them, a hidden node that nobody writes; each side then writes into its own store, and a write to a shared page copies it out first. That is why a clone does not hang the new node under the old one but inserts a new node between the old one and its parent, and moves the old node's pages up into it.

The second thing is the share count, which is what will later let a write skip the copy. A page a hidden node holds is seen by some number of leaves; when a leaf writes it, the leaf must copy if any other leaf still sees it, and may take the page itself if it is the only one left. A count of "leaves that see it, minus one" answers that with one comparison, and it is incremented for exactly the pages a new clone can reach, once each, by walking from the clone upward the way a read does, after the tree is settled, so that what is counted is what a read of the clone finds.

**Change**
After the two allocations, which are the only thing that can fail:

```cpp
hidden_node->m_pages.take_all_from(m_pages);   // a swap of two trees: no page moves
m_parent->m_children.erase(*this);             // this node's place in its parent's list
m_parent->m_children.push_back(*hidden_node);  //   goes to the hidden node
m_parent = hidden_node;                        // this node now hangs under it
m_parent_offset = 0;
m_parent_limit = m_size;
hidden_node->m_children.push_back(*this);
hidden_node->m_children.push_back(*clone);
```

When the node holds nothing in the window and its parent is already hidden, the search goes up (`FindParentAndRangeForCloneLocked()`) and the clone hangs under that hidden node, translated into its coordinates; no new node is made. That is what keeps a program started many times, which is the launcher's case, at one hidden node with many leaves, not a chain that a lookup would have to climb.

`VmCowPages` does not know the `VmObject`s over it, so `create_clone_locked()` cannot remove the write permission from the source's mappings, as Zircon's clone does, or refuse a pinned page (pinning is per object here). Both are documented on the function as the caller's to have seen to; the `VmObject` layer that calls it does it, in the step that adds that layer. Nothing else calls it until then.

Rejected: keeping the share count in the hidden node, in a table by page number. The descriptor has room, Zircon keeps it there, and a table would need a lookup on every write and its own allocation, which a clone that must not fail half way cannot afford.

**Reference**
Zircon `VmCowPages::CreateCloneLocked()` (`vm_cow_pages.cc:1831`), with `FindParentAndRangeForCloneLocked()` (`:1526`), `CloneNewHiddenParentLocked()` (`:1582`, its `page_list_` move at `:1730`), `CloneChildLocked()` (`:1782`, the hang-under-existing case), and `vm_page_t::object.share_count` (`page.h:72`). Zircon also removes Write from the source's mappings first (`RangeChangeUpdateLocked`), and rewrites each moved page's back-pointer through the page queues; ours does neither, for the two reasons above and because no VMO page has a back-pointer.

**Verification**
- `scripts/format_cpp.py --check`: clean. `clang-tidy -p out/vmo-snapshot`: nothing on the changed lines; the one warning is the existing destructor without `override`.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts as before. dbg: 18 passed, 0 failed, 0 error lines. rel: 13 passed, 9 skipped, 0 failed, 0 error lines.
- `vmosnapshot.test` over COM1: 74 checks ok on both images (44 before, 30 new): a node that holds pages is cloned and both hang under one new hidden node with the pages moved up and one more viewer counted; a second clone hangs under the same hidden node with no new node; a one-page window counts only its page; a clone longer than the node reads zero past its end; a clone of a clone hangs under the same node, six under it; a misaligned offset or size and a clone of a hidden node are refused; a node with no pages still gets a hidden node above it; the hidden node takes the node's place on its parent's list, which still lists one child; with every other handle dropped the last clone still reads the pages. `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel, still pass.
- The test can fail. Three mutants were built and each failed the checks that name it: with no page move, eight checks; with no share count, four of 74; with the reuse of an existing hidden node turned off, two of 74. (A first attempt at the count mutant did not compile, and its "ok" was the previous binary; it was redone with a mutant that builds.)
- Not exercised: a node that holds its own pages under a hidden parent (a node cannot get one until a write copies it), the counts after any node dies or writes (neither is written yet), a clone that runs out of memory, and concurrency. The test is single-threaded.

**Deliberately not done**
- No write path: the fork-or-migrate write, and the drop of a share count, are the next step. Counts only go up until then.
- No merge when a node dies: a hidden node left with one child stays.
- No `RemoveWrite` on the source's mappings and no refusal of pinned pages; see above.
- No rollback of counts on failure, because nothing fails after the counting starts: the allocations are done first.

**How it was found**
None: a step of the design. The walk that counts was moved from the cursor onto the node so that the clone can use exactly the walk a read uses; a test of the offset arithmetic (an earlier mutation of the walk) covers both.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a clone insert a hidden node above the source instead of hanging the clone under it, and what would the clone see if it hung under the source?
- Why does the second clone of the same node not make a new hidden node, and what would a program started a hundred times cost if it did?
- What does a share count of zero on a page in a hidden node tell a writer, and why is it counted after the tree is settled and not before?

## Title: A VmCowPages node's kind is a group of flags, not a bool (2026-09-30)

**Files**
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — `VmCowPages::Flags` (`None`, `Hidden`); `create_child()`, the constructor and `make_node()` take a `Flags`; `m_hidden` becomes `m_flags`, and `is_hidden()` tests the bit.
- `kernel/mm/vm_cow_pages.cpp` — the callers, and every read of `m_hidden` now goes through `is_hidden()`.
- `kernel/commands/vmo_snapshot_test.cpp` — the test's helper and one direct call pass a `Flags`.

**Symptom**
None. Suggested by the user while reading the header: a node will have more attributes, and each one as its own bool would have to be threaded through every factory by hand.

**Cause**
`bool hidden` was the first attribute of a node, and it was spelled as a bool all the way down: a member, a constructor parameter, a parameter of `make_node()` and of `create_child()`. A second attribute would have added a second `bool` beside it in each of those four places, and two adjacent bools of a call are the kind of arguments that get swapped without the compiler noticing. An `enum class` bitmask has one parameter however many attributes there are, and the call site names the one it sets (`Flags::Hidden`) instead of passing `true`.

**Change**
```cpp
enum class Flags : u8 {
    None = 0,
    Hidden = BIT(0),
};
USTL_ENABLE_INNER_ENUM_BITMASK(Flags);
...
Flags const m_flags;
auto is_hidden() const -> bool { return !!(m_flags & Flags::Hidden); }
```
The shape is that of `VmObject::Flags`, which is what `VmoFlags` names. The type-safety paid for itself at once: the compiler refused the one call in the test that still passed `false`.

**Reference**
None, this project's own design: Zircon keeps the same attribute as the `is_hidden()` test on a node's kind, and its other attributes are `VmCowPagesOptions` bits, an `enum class` bitmask as well.

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the changed lines.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts. `vmosnapshot.test` 74 checks ok on both images; `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg. No behaviour changed, and the check that would notice is the test's 74, which builds hidden nodes and clones of them.
- The first build after the edit failed on the test's stale `false`; the tests that ran in that same command ran on the previous binary, and were rerun after the fix, on images newer than the change.

**Deliberately not done**
- No second flag: none is needed yet.
- The test's own helper keeps its `bool hidden` argument and translates it, so the sixty-odd call sites did not have to change; it is a helper, not the interface.

**How it was found**
The user's suggestion, made on the line `bool const m_hidden;`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is one `Flags` parameter safer than one `bool` per attribute once there are two attributes?
- What did `enum class` refuse at compile time here that a `bool` would have accepted?
- Why does `is_hidden()` need no lock, when other reads of a node do?

## Title: A write to a page a node sees only through a hidden node copies it, or takes it (2026-09-30)

**Files**
- `kernel/mm/vm_cow_pages.cpp`, `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — `VmCowPages::cow_page()`; `find_ancestor_page()` now returns the page with the node that holds it and its offset there (`AncestorPage`); `Cursor::require_owned_page()` calls it instead of refusing.
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `PageFrame::drop_share()`.
- `kernel/mm/include/ours/mm/vm_page_map.hpp` — `remove_page()`.
- `kernel/commands/vmo_snapshot_test.cpp` — 12 more checks, 86 in all.
- `docs/repairs/260930/mm.md` (new) — the fork zero-fills a frame it then overwrites.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — this step done.

**Symptom**
None. Latent, cannot be triggered today: nothing but the test makes a node with a parent. It is the second half of the copy-on-write child: a write to a page the node sees through a hidden ancestor used to answer `Unimplemented`, and would otherwise have been answered with a fresh zeroed page, which silently throws away what the ancestor holds.

**Cause**
A page shared through a hidden node cannot be written by any of the nodes that see it, and each of them must still be able to write it as if it were its own. The two ways to give a writer a page of its own are a copy, which is always right, and taking the page itself, which is right only when the writer is the last node that sees it. The share count is what tells them apart, and it is why the count exists: with it a program started once and written to often does not copy every page it writes, and without it every first write would.

What decides the case is one number read and one number written, and in between the copy is made. That is why all of it has to happen under one hold of the tree's lock. Two writers that each read "one other viewer" and then both fork would drop the count from one to below zero, and, unsigned, to four billion, so no writer would ever see zero again and the page would stay in the hidden node for good. A writer that reads zero while another still reads the page would take a page the other keeps seeing, and the taker's writes would show there: two processes sharing writable memory that neither asked for, with no error anywhere.

**Change**
```cpp
if (holder->is_hidden() && 0 == source->share_count()) {
    insert into this node; then remove from holder;   // a migrate: nothing is copied
    return source;
}
allocate a frame; copy PAGE_SIZE bytes; insert into this node;   // a fork
if (holder->is_hidden()) source->drop_share();
return copy;
```
Everything that can fail comes first: the frame is allocated and put into this node's store before the count is dropped or the page is taken out of the hidden node, and the page is inserted into this node before it is removed from the other, so a failed insertion leaves it where it was. A page a node that is not hidden holds is only ever copied and no count is touched, because that node can still write it; taking it would steal a live node's page. A page with nothing above it, a hole or the zero tail, is not shared content at all and is allocated as a hole is.

`drop_share()` asserts the count is above zero, for the reason above.

Rejected: dropping the lock between the decision and the copy, as `copy_page_by_page()` does around a user copy. That function has a user buffer that can fault; this has none, and the window is exactly where the two failures above live.

**Reference**
Zircon `VmCowPages::CloneCowPageLocked()` (`vm_cow_pages.cc:2671`): a share count above zero forks with `AllocateCopyPage()` and decrements, zero migrates the page from the hidden owner, and the slot is reserved first so that an allocation failure changes nothing. Zircon also unmaps the writer's old read-only mapping on a fork; ours does not know the mappings, so that belongs to the fault path.

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the changed lines (the existing destructor warning only).
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts as before. dbg: 18 passed, 0 failed, 0 error lines. rel: 13 passed, 9 skipped, 0 failed, 0 error lines.
- `vmosnapshot.test` over COM1: 86 checks ok on both images (74 before, 12 new). One node with two clones: the count is two; the first writer forks and the page stays; what it writes does not reach the others; it reads its own page afterwards; the second writer forks and the count is zero; the last node takes the page and the hidden node lets go; on page 1 in the other order two forks and then a take; a write to a hole and a write to the zero tail allocate a zeroed page; a page a node that is not hidden holds is copied and its count is left alone. `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel, still pass.
- The test can fail. Three mutants: taking the page whatever the count, five of 86 checks fail (among them that a write reaches the page the others read); never dropping the count, four; never taking the page, the kernel stops on `drop_share()`'s assertion, "a page with no other viewer lost one", before the test ends, which is the assertion doing what it is there for.
- Not exercised: a failure part way (there is no way to make an allocation or an insertion fail on demand), a second CPU, and the mappings: no node with a parent is behind a mapping, so the write fault of a read-only page is not reached. The same writes through `VmObject::read`/`write` wait for the `VmObject` layer.

**Deliberately not done**
- No unmap of the writer's old read-only mapping on a fork: the mapping step.
- No merge when a node dies: counts only go down through writes, and a node that dies without writing leaves its count behind, which the death step fixes.
- No skipping of the zero-fill of the frame a fork overwrites; recorded in `docs/repairs/260930/mm.md`, to be measured before it is changed.
- No lock-free path. One mutex per tree is decision 3 of the design and this is the code that holds it longest, a page copy under it, which is what the benchmark after the fault step will measure.

**How it was found**
None: a step of the design. The two failures under Cause are the ones named in the user's answer to the invariant exercise ("the count and the number of viewers must agree"; two writers reading the count and both dropping it) and the one that answer did not name, a taker racing a reader.

**The user's prediction**
"The shared count and the number of objects that actually hold the page must agree. Without a lock A and B can reach the count one after the other, and both delete, making the count negative." Graded: the invariant is right but it is the number of leaves that can *see* the page, minus one, not the number that hold it (only the hidden node holds it); the underflow is right but unsigned wraps instead of going negative, which is worse because nothing notices; and the more dangerous interleaving is a taker racing a reader, which breaks isolation without any visible symptom.

**Three questions you should be able to answer**
- Why is "a write to a page in an ancestor" the test for copy-on-write, and what does the share count decide once that test is true?
- Why must the count be read, the copy made and the count written under one hold of the lock, and what are the two ways it fails if they are not?
- Why is a page held by a node that is not hidden never taken, whatever its count?

## Title: One find_page() answers what a node sees, own page or an ancestor's (2026-09-30)

**Files**
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `find_ancestor_page()` becomes `find_page()`, which looks in the node's own store first and then walks up; `AncestorPage` becomes `FoundPage`. `Cursor::require_read_page()` and `require_owned_page()` each ask it once.

**Symptom**
None. Suggested by the user while reading the code: no `find_ancestor_page`, just `find_page`, with the two lookups in one function.

**Cause**
The question a cursor asks is "what does this node see at this offset", and the code answered it in two pieces, in two places each: look in the node's own store, and then, if that failed and the node had a parent, call a second function that walked the ancestors. The pieces were written in that order because the walk was added after the own-store lookup, not because they are two questions. Split like that, every caller had to remember to do both, in that order, and to work out afterwards which one had answered, since the answer decides whether the page may be written. One function returning the page with the node that holds it gives that decision to a comparison, `found.owner == this`, and leaves no caller able to skip the first half.

**Change**
```cpp
while (true) {
    if (auto *const page = node->m_pages.get_page(PgOff(offset >> PAGE_SHIFT))) {
        return {.page = page, .owner = node, .offset = offset};
    }
    if (!node->m_parent || offset >= node->m_parent_limit) {
        return {.page = nullptr, .owner = nullptr, .offset = 0};
    }
    offset += node->m_parent_offset;
    node = node->m_parent.as_ptr_mut();
}
```
A read is writable when the page is the node's own and the node is not hidden; a write that finds its page anywhere but in the node's own store goes to `cow_page()`; a write that finds nothing is allocated. The own-store hit costs what it cost, one lookup. `create_clone_locked()` counts through it too: the clone is new and its own store is empty, so what it finds is what it can see above.

**Reference**
Zircon `VmCowPages::FindPageContentLocked()`, which also looks in the node first and then in its parents in one loop, and `LookupCursor::PageAsResultNoIncrement()`, where `writable` is true only when the page is in the target itself.

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the changed lines (the existing destructor warning only).
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts. `vmosnapshot.test` 86 checks ok on both images, unchanged; `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel. No behaviour changed, and the 86 include every case that reaches this code: own page, ancestor page, zero, hole, forks and takes.
- Not run: the mutation checks of the earlier entries; nothing they cover was touched but the shape of the lookup.

**Deliberately not done**
- The message the user wrote was short, and I read "two logic in one function" as the own-store lookup and the ancestor walk becoming one function. If it meant something else, this is the part to change.

**How it was found**
The user's review comment.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What decides whether the page `find_page()` returns may be written, and why is it the node that holds it and not the page?
- Why does a node with no parent use the same function, and what does it cost there?
- Why does the clone's counting loop find only ancestors' pages, though it calls the function that also looks in the node's own store?

## Title: A test for two windows of one node that do not overlap (2026-09-30)

**Files**
- `kernel/commands/vmo_snapshot_test.cpp` — 9 more checks in `vmosnapshot.test`, 95 in all.

**Symptom**
None. The case was asked about, not seen fail: several clones of one node, each looking at a range of pages the others do not, which is what a launcher does with a boot image that holds many programs' files. The tests had one window, and every other clone looked at the whole node.

**Cause**
The share count, the copy-or-take decision and the offset arithmetic all work per page and per window, and a test in which every clone sees the same pages cannot tell a count that is kept per page from one that is kept per node, or an offset that is composed from one that is only ever zero. Two disjoint windows are the smallest case in which a page's count depends on which clone can reach it, and in which a write in one window must leave the other's pages and counts alone.

**Change**
A six-page image with two windows, pages 1-2 and 4-5, hung under one hidden node. The checks: the hidden node lists three children and holds all six pages; each window's pages have a count of one and the two pages in neither have zero; each window reads its own pages of the image; a write in one window forks its page and leaves the other's untouched; the image, which sees every page, takes the pages nobody else still sees, one at a time, and the pages a window still sees stay shared; and the windows still read the pages that stayed.

**Reference**
None, this project's own test. The case is the one the launcher will produce: `vmo_create_child(bootfs, offset, size)` for each program's segments of one image.

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the test.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts. `vmosnapshot.test` over COM1: 95 checks ok on both images (86 before, 9 new).
- The new checks can fail: with the composition of the parent offset removed from the walk, 12 of 95 checks fail, six of them among the new nine (the counts, each window's reads, both writes, the image's takes and the reads after them); the line was restored and all 95 pass.

**Deliberately not done**
- No lock contention measurement. The two windows share the tree's lock although their pages are disjoint; that is decision 3 of the design, to be measured with the benchmark after the fault step.
- No death of a window: a window's pages keep their count when it dies until the death step.

**How it was found**
The user's question, whether a hidden node's observers could each look at a completely different range.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the image, which is not a clone, count as a viewer of every page it has cloned windows of?
- After both windows have written their first page, why does the image take pages 0, 1 and 3 but not 2 or 4?
- Why do two windows with no page in common still contend on one lock, and where is that decided?

## Title: A clone that runs out of memory half way no longer takes the tree's lock twice (2026-09-30)

**Files**
- `kernel/mm/vm_cow_pages.cpp` — `create_clone_locked()` makes the new hidden node with no parent and gives it this node's parent only in the part that cannot fail.

**Symptom**
Latent, cannot be triggered by the tests: it needs an allocation to fail. When the node being cloned has a parent, and the allocation of the hidden node succeeds and the allocation of the clone then fails, `create_clone_locked()` returned with the caller holding the tree's lock, freed the hidden node, and the hidden node's destructor took that same lock. The mutex is not recursive, so the thread would wait for itself for ever.

**Cause**
A `VmCowPages` that has a parent takes the tree's lock in its destructor, to take itself off the parent's list. `create_clone_locked()` runs with that lock held by its caller, and it made the hidden node with the parent already set, so freeing it on the failure path was destroying a node that has a parent while holding the lock its destructor wants. The bug is the general one: the last reference to a node must never be released with the tree's lock held, if the node has a parent. On the success path nothing is freed, and the node with no parent (a root being cloned, which every test does) never takes the lock, so it did not show.

**Change**
The hidden node is made with an empty parent, offset and limit, and given this node's in the part after the last allocation, where nothing can fail:

```cpp
make_node(m_gaf, m_size, m_hierarchy, ustl::Arc<VmCowPages>{}, 0, 0, Flags::Hidden, &hidden_node)
...
// Nothing fails from here.
hidden_node->m_parent = m_parent;
hidden_node->m_parent_offset = m_parent_offset;
hidden_node->m_parent_limit = m_parent_limit;
```
A failure now frees a node with no parent, whose destructor takes no lock.

Rejected: having the destructor look at whether the node is linked before it takes the lock. That reads the hook without the lock, which the merge in the next step will change under it.

**Reference**
Zircon returns the parent it would drop to the caller, so that it is released after the lock (`MaybeDeadTransition()` returns a `RefPtr`, `vm_cow_pages.cc:1022`); the same rule, applied by handing the reference out instead of by not holding it yet.

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the changed lines (the existing destructor warning only).
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts. `vmosnapshot.test` 95 checks ok on both images.
- Not verified: the failure path itself. There is no way to make an allocation fail on demand, so the fix rests on the reading above and on no success path having changed, which the 95 checks cover.

**Deliberately not done**
- No allocation-failure injection, which would make this and the other "nothing fails from here" claims testable, and is a larger piece of work.
- The rule for the merge in the next step (never release the last reference to a node under the lock) is written in the design, not enforced.

**How it was found**
Judging whether any real scenario has a walker retain a node that is being destroyed: it does not, but the same question asked of the other half, what frees a node while the lock is held, found this one.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does releasing the last reference to a node that has a parent, while holding the tree's lock, never return?
- Why did no test show it, and what would have?
- Why is the hidden node given its parent only after the last allocation?

## Title: Releasing the last reference to a VmCowPages under its tree's lock stops the program with a message (2026-09-30)

**Files**
- `kernel/mm/vm_cow_pages.cpp` — `~VmCowPages()` asserts, in every build, that the thread destroying a node with a parent does not hold the tree's lock.
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — the comment at `m_children` now says what is enforced, and where.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — the open question about walking the list of children, restated.

**Symptom**
Latent. A `VmCowPages` with a parent takes the tree's lock in its destructor, so releasing its last reference on a thread that already holds that lock waits for itself for ever; lockdep reports it only in the builds that have it. One instance was found and fixed the same day (the clone's failure path); the next is whoever writes a code path that drops a node while holding the lock, most likely the merge of the next step.

**Cause**
The rule that was written down, "never release the last reference to a node under the lock", is one the next person can break without noticing, and a hang in a release image says nothing about where it happened. The user asked that it not depend on being remembered.

**Change**
```cpp
ASSERT(m_lock.holder() != task::Thread::Current::get(),
       "the last reference to a VmCowPages was released with its tree's lock held");
```
before the destructor takes the lock. It uses the mutex's own record of who holds it, so it is exact, not a guess, and is on in every build. It is a failure that names its cause, not a prevention: a node cannot be freed without its lock, and deferring the free to another thread (the `rc_recycle()` hook and a worker would do it) is a larger mechanism that nothing needs while nothing but the merge drops nodes under the lock, and the merge is written to hand its references out.

The other half, that a walker of the list may not bring a dying node back to life, is closed in the `Arc` itself (`docs/changes/260930/ustl.md`): `retain()` stops on it, and `Arc::try_adopt()` is the way to ask.

Checked by a one-off experiment, not kept in the tree: a node made under a parent was released inside a `Guard` of its own lock. Both images stopped with `Assertion failed on m_lock.holder() != task::Thread::Current::get() with message: the last reference to a VmCowPages was released with its tree's lock held`, where a hang would have been.

**Reference**
Zircon: `MaybeDeadTransition()` returns the parent `RefPtr` it would drop, so the caller releases it after the lock (`vm_cow_pages.cc:1022`); its `~VmCowPages()` also asserts the node is off every list. Linux: a mutex is not recursive either, and `lockdep` reports the same mistake as a possible recursive locking.

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the changed lines (the existing destructor warning only).
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts. `vmosnapshot.test` 95 checks ok on both images; `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel.
- The experiment above, on both images.
- Not tested in the tree: the stop, since it ends the kernel.

**Deliberately not done**
- No deferral of the destruction to a worker; see Change.
- No test that stops the kernel.

**How it was found**
The user's review: the safety must not rely on the next person writing it right.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the destructor need the tree's lock only for a node that has a parent?
- What does the assertion do that lockdep does not, and why is it on in every build?
- Why is stopping the failure the merge must not reach, and what does the merge do so that it does not?

## Title: A leaf that dies lets go of what it sees, and a hidden node left with one child is merged away (2026-09-30)

**Files**
- `kernel/mm/vm_cow_pages.cpp`, `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — `~VmCowPages()` runs `release_viewed_pages_locked()` and, if its parent is then left with one child, `merge_into_only_child_locked()`.
- `kernel/mm/include/ours/mm/vm_page_map.hpp` — `next_page()`.
- `kernel/commands/vmo_snapshot_test.cpp` — 19 more checks, 114 in all.
- `docs/repairs/260930/mm.md` — item 2, the cost of the walk.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — this step done.

**Symptom**
None. Latent, cannot be triggered today: nothing but the test makes a tree. Without this step every page a clone ever saw stays in its hidden node until the whole tree is gone: a program started a thousand times and closed a thousand times would keep the pages it copied from and the hidden node it shared, for good, and every lookup would climb through nodes with a single child.

**Cause**
The share count says how many leaves see a page, and a leaf that dies is one fewer, so the count has to drop when it does, or the page is never seen as unshared again: the last viewer would still be told to copy it, not to take it. And when the count would go below zero the last viewer has died and the page is nobody's and has to be freed, which is the only place a page in a hidden node is ever freed before the hidden node is. A hidden node with one child is a step every lookup below it climbs for nothing, and the child's own pages and the pages it shares with nobody else can live in one node. The merge is what makes the tree shrink back to what is live.

Two things the merge relies on, and one thing it cannot do. It relies on every dying leaf having already dropped its counts, under the same hold of the lock and before it leaves the list: so by the time a hidden node has one child left, the only pages in it with a count above zero are ones a node above it also sees, and the pages the survivor sees are its alone. It relies on the survivor being reachable only through the list, so it changes the survivor's fields and never takes a reference to it (the survivor may itself be dying, with its destructor waiting for the lock). And it cannot fail in the middle: moving a page into the survivor is the one step that allocates, so it goes into the survivor before it comes out of the hidden node, and if it fails the merge stops where it is, which is a consistent tree: the pages moved are the survivor's, the rest are still seen through the hidden node, and it stays a step until its child dies.

**Change**
```cpp
release_viewed_pages_locked();                 // counts down, last viewer frees
if (m_sibling_hook.is_linked()) { m_parent->m_children.erase(*this); }
if (m_parent->is_hidden() && 1 == m_parent->m_children.size()) {
    m_parent->merge_into_only_child_locked();  // pages up into the survivor, parent link composed
}
```
The merge moves each page of the hidden node the survivor sees and that the survivor holds no page of its own over, frees each one nobody sees, and gives the survivor the hidden node's parent, with its offset the sum of the two offsets and its limit cut where the hidden node's window in its parent ends. The survivor takes the hidden node's place on its parent's list, so the parent's count of children does not change and no merge cascades. The hidden node then has no parent, no children and no pages, and is freed by whoever holds the last reference, which is the dying node the merge runs in: its own release of the reference is what frees it, after the lock, and it takes no lock because it has no parent.

The counting rule is the same as `create_clone_locked()`'s, run backwards: the pages a leaf is a viewer of are the ones a walk from it finds in a hidden node, at offsets where it has no page of its own.

**Reference**
Zircon `VmCowPages::ReleaseOwnedPagesRangeLocked()` (`vm_cow_pages.cc:2842`), which frees the pages with a share count of zero and decrements the rest; `RemoveChildLocked()` (`:2030`) and `MergeContentWithChildLocked()` (`:2117`), which merges a hidden node with one child left into it and which can fail only on an allocation failure, leaving the node until its child dies.

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the changed lines (the existing destructor warning only).
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts. `vmosnapshot.test` 114 checks ok on both images (95 before, 19 new); `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel.
- The new checks: a node with two clones, one clone dies, each page has one viewer fewer and the hidden node with two children left stays; the other dies, the hidden node is merged away, the node that is left has no parent and holds the very pages, writable, not copies. A page seen only by the last clone: the other two write it, the last clone dies and the page is freed from the hidden node, which stays for the two left. The source dies first: a one-page window is left with its page, moved, and no parent. A window at page 1 writes a page and is cloned in turn, which gives it a hidden node of its own below the first; the clone dies and the node between is merged away with the offsets composed: the window hangs under the first again, still reads its own page and the page the composed offset points to, and the counts are as if the clone had never been.
- The checks can fail. Four mutants: no viewer accounting stops the kernel on the merge's own assertion, "a page with another viewer in a hidden node with one child", which is the invariant the merge relies on; no merge fails five of 114; offsets not composed fails one; the last viewer not freeing the page fails one. (One first attempt did not match the text it meant to change and its "ok" was the unmutated build; it was redone.)
- Not tested: a page insertion that fails in the merge, a second CPU, and the frame of a freed page: the tests see a page leave its hidden node, and that a page is freed rests on the line that follows it.

**Deliberately not done**
- No lock-free or deferred destruction; the node that is merged away is freed by the node whose destructor runs the merge, and takes no lock.
- No faster walk: the death visits every offset of its window, recorded in `docs/repairs/260930/mm.md` for measurement.
- No accounting for a node under a parent that is not hidden: such a parent's pages belong to a live node and have no count. Only a test builds one.

**How it was found**
None: a step of the design. The rule about not taking a reference to the survivor came from the user's review of the list of children, and the guards for it are in the `Arc` and the destructor, from the entries before this one.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must a dying leaf drop its counts before it leaves the list, and both in the same hold of the lock?
- Why does the survivor take the hidden node's place on the list, and what would a count that changed there cause?
- Why does a failed insertion in the merge leave a tree that is still correct, and what does it cost?

## Title: A mapping's protection ranges are written under the object's lock as well as the address space's (2026-09-30)

**Files**
- `kernel/mm/vm_mapping.cpp` — `protect_locked()` and `unmap_locked()` take the object's lock for their whole body.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — step 8 split into four commits (8a to 8d), and steps 9 to 11 renumbered.

**Symptom**
Latent, cannot be triggered today: nothing reads a mapping's protection ranges without the address space's lock. The reader this prepares for is the clone, which has to remove Write from every mapping of the pages it is about to share.

**Cause**
The clone runs on an object, holding the object's lock, and the mappings of that object belong to address spaces whose lock is taken first: aspace lock, then object lock, on every path the fault handler takes. So from the object's side the address space's lock cannot be taken to look at a mapping, and the mapping's record of its permissions, `m_protection_ranges`, was guarded by that lock alone. Two locks that do not know of each other over one piece of data is the whole problem: the clone reads the ranges while a `protect()` rewrites them.

The fix is to have the writers hold the object's lock too, and the reason it has to be the whole of each function and not just the update of the record is an interleaving. `protect_locked()` updates the record to include Write, and then applies Write to the page tables. If the clone slips between the two, it reads the new record, removes Write from the page tables, and the `protect_locked()` then applies Write to them again: a page that became shared, writable in one mapping for good. Holding the object's lock across both makes the record and the page tables change together as far as the clone can see.

The lock order stays aspace, then object, then the page table's own. That last is not new: `PageTable` already has its own innermost spin lock, taken with interrupts off, in `map_pages()`, `unmap_pages()`, `protect_pages()`, `query_mapping()` and `destroy()` (`kernel/arch/x86/mm/page_table.cpp`), which is what Zircon's `X86PageTableBase::lock_` is. I said earlier in this design that `ArchVmAspace` had no lock of its own; that was wrong, I had read the wrapper and not the page table under it.

**Change**
```cpp
DEBUG_ASSERT(m_vmo);
Guard guard{m_vmo->lock()};
```
at the top of `protect_locked()` and of `unmap_locked()`. Neither is called with the object's lock held: `VmMapping::destroy()` takes it only around its own removal from the list, and calls the arch unmap directly, and every other caller holds only the address space's.

**Reference**
Zircon `VmMapping::EnumerateProtectionRangesLockedObject()` and `AspaceRemoveWriteLockedObject()` (`vm_mapping.cc:699`): the mapping's protection ranges are read under the object's lock, and the remove-write takes no address space lock, which is safe because the mapping is on the object's list, in the ALIVE state, only while the object's lock is not held by whoever removes it.

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the changed lines (two old `(void)` casts are reported near them).
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts, and the debug image has lockdep on, which reports a wrong order as an error and reported none. `vmosnapshot.test` 114 checks; `usercopy.test` 18 and `vmaseal.test` 13 on both images, `pmnode.test` 20 on dbg. The shell's `vma_map`, `protect` and `unmap` checks run through both functions in every image.
- Not verified: that the lock is what excludes the reader, because there is no reader yet. The clone's remove-write is the next step, and its test will be the first to hold the object's lock and read the ranges.

**Deliberately not done**
- No reader. The remove-write is 8b.
- No change to what the fault path takes: it reads the ranges holding the address space's lock, which no writer can hold without it.

**How it was found**
Working out how the clone removes Write from other mappings: the user chose Zircon's way over a counter of writable mappings, and reading Zircon's `AspaceRemoveWriteLockedObject()` gave the requirement that its protection ranges be readable under the object's lock alone.

**The user's prediction**
None: asked for the invariant argument (which two things must agree in the ranges, and what does not agree when the writer holds only the address space's lock and the clone only the object's) and the user said to go on. The intended answer: the record of a mapping's permissions and the permissions in its page tables must agree whenever no one holds the lock; and with two different locks the clone can read the record between a `protect()`'s update and its application to the page tables.

**Three questions you should be able to answer**
- Why must `protect_locked()` hold the object's lock across the page tables too, and not only across the update of the record?
- Why can the clone not simply take the address space's lock to read the record?
- What is the lock order these three locks make, and which path already used it?

## Title: Write can be taken away from the mappings of an object's pages, by an operation that only narrows (2026-09-30)

**Files**
- `kernel/lib/arch/include/arch/paging/mod.hpp` — `Paging::restrict()` and its `RestrictVisitor`; `protect()` and `restrict()` now share one walk, `visit_range()`.
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp`, `kernel/arch/x86/mm/page_table.cpp` — `PageTable::restrict_pages()`.
- `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp`, `kernel/arch/x86/mm/arch_vm_aspace.cpp` — `ArchVmAspace::restrict()`.
- `kernel/mm/vm_mapping.cpp`, `kernel/mm/include/ours/mm/vm_mapping.hpp` — `VmMapping::remove_write_locked_object()`.
- `kernel/mm/vm_object.cpp`, `kernel/mm/include/ours/mm/vm_object.hpp` — `VmObject::remove_write_locked()`.
- `kernel/commands/vmo_snapshot_test.cpp` — 13 more checks, 127 in all, through real mappings and the page tables.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — 8b done, 8b' added.

**Symptom**
Latent, cannot be triggered today: nothing calls `remove_write_locked()` yet. It is the half of the clone that makes pages safe to share: a page that another node is about to see must not stay writable through a mapping that will not know.

**Cause**
When a clone moves the pages of a node into a hidden node above it, the node's own mappings still have those pages in their page tables, and if the entries are writable a write goes straight to a page that every other viewer now shares, with no fault, no copy and no error. The fix is to take Write from those entries and to leave each mapping's record of what it allows untouched: the mapping is still allowed to write, but a write now faults and is resolved by the copy-on-write path.

The way it is done is the user's rule. `protect()` sets the permissions of the entries in a range to a value, so given the permissions a range may have it also gives back what an entry lacks: a page mapped read-only because it is shared comes out writable. An operation that is meant to narrow cannot be built on that. `restrict()` is the primitive that only narrows: each entry becomes what it was AND what is kept, on the permission bits only, and an entry that already has less keeps what it has. Widening is then left to the one place that can show a page may be widened, the fault path after the page has been shown to be the writer's own. The same primitive serves the clone here and, in the next commit, `protect()` itself.

**Change**
```cpp
auto const kept = (current & ~MmuFlags::PermMask) | (current & m_keep & MmuFlags::PermMask);
entry = entry.with_protection(MmuFlagsConv<ArchMmuFlags>::to_arch(kept));
```
`remove_write_locked_object()` finds the part of the given range that the mapping shows (the mapping's offset into the object, its base and size, none of which change after it is made), walks its protection ranges, and for each region that has Write calls `restrict()` with everything but Write. It takes no address space lock: the mapping is on the object's list and cannot be destroyed, the fields it reads do not change, and the ranges are written only under the object's lock since the previous commit. It stops the program if `restrict()` fails, since a failure would leave a shared page writable.

Rejected: `protect()` with the region's flags less Write, as Zircon does. Right for narrowing a whole region, and wrong for a region whose entries are already lower: it would set them.

**Reference**
Zircon `VmObject::RangeChangeUpdateLocked()` with `RemoveWrite`, which calls `VmMapping::AspaceRemoveWriteLockedObject()` (`vm_mapping.cc:699`); the entry is `ProtectOrUnmap()`.

**Verification**
- `scripts/format_cpp.py --check` clean on all ten files; `clang-tidy` nothing on the changed lines (the one warning reported is an old `virtual` destructor).
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts. `vmosnapshot.test` 127 checks ok on both images (114 before, 13 new); `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel.
- The new checks build an address space, map committed VMOs, and read the page table entries back: Write is taken from one page of three and from no other; the entry is exactly what it was, less Write (in particular nothing is added); doing it again, or for a range past the object, changes nothing; a read-only mapping and another object's mapping are not touched; a mapping with no entries is left with none; a length that reaches the end of the address space takes Write from every page; and two mappings of one object at different offsets each lose Write on the address that shows the page.
- The checks can fail. Three mutants: taking nothing away fails three of 126, the address computed from the wrong offset fails four, and the primitive setting the permissions to what is kept instead of clearing fails one, the exact-entry check, which a first version of the test missed (it only asked whether the entry was writable, and the mutant only added Execute).
- Not verified: another CPU's translation of an entry that lost Write. `restrict_pages()` invalidates the local TLB only, as `protect_pages()` does; this is the first open question of the design, and it now has a concrete failure to point at.

**Deliberately not done**
- No callers: `create_child_snapshot()` (8d) calls it. The next commit makes `protect_locked()` use `restrict()`.
- Slices: a slice shares its parent's pages and has its own list of mappings, so removing Write from one object's mappings does not reach the other's. That is 8c.
- No shootdown to other CPUs.

**How it was found**
The user's question whether `protect` can still widen, and the rule that it must only narrow.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `protect()` widen a page that is mapped read-only because it is shared, and how does `restrict()` not?
- Why is it correct to leave the mapping's record with Write when the entries have lost it?
- Why is it safe to do this holding the object's lock and not the address space's?

## Title: protect() only narrows, and the fault path is the one place a permission is added (2026-09-30)

**Files**
- `kernel/lib/arch/include/arch/paging/mod.hpp` — `ProtectVisitor` clears bits (AND) instead of setting them; `restrict()` and `RestrictVisitor`, added in the entry above, are gone.
- `kernel/arch/x86/kernel/include/ours/arch/x86/page_table.hpp`, `kernel/arch/x86/mm/page_table.cpp`, `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp`, `kernel/arch/x86/mm/arch_vm_aspace.cpp` — `restrict_pages()` and `ArchVmAspace::restrict()` removed; `protect` documented as narrowing.
- `kernel/mm/vm_mapping.cpp`, `kernel/mm/include/ours/mm/vm_mapping.hpp` — `remove_write_locked_object()` calls `protect()`; `page_fault()` widens through the new `widen_present_locked()` and `remap_locked()`, and maps a page that is not the node's to write without Write.
- `kernel/commands/vmo_snapshot_test.cpp` — 15 more checks, 142 in all, through real faults.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — 8b and 8b' done.

**Symptom**
Latent, cannot be triggered today: no mapping sees a shared page yet. Before this, `protect()` set the permissions of the entries in a range to the value it was given, so a `protect()` of a mapping to the permissions it already had would give Write back to a page that had lost it, and the clone that made those pages shared would be undone by anyone who called it. The fault path did the same, in its branch for an entry whose permissions differed from the region's.

**Cause**
The user's review of the previous entry: I had added `restrict()` beside `protect()` to have an operation that only narrows, and left `protect()` able to widen. Two operations for one job, and the wrong one left available. "Protect" is a narrowing verb, and a page-table operation that can also widen is a hazard for the next caller, who will pick it. So the semantics moved into `protect()` itself and the second operation went away.

What that leaves is a question: how does a permission ever get added? By mapping the page again, unmap and then map, because that names the frame, and whoever names the frame has to have shown the page may have the permission. That is one place, `widen_present_locked()`: a write to an entry without Write asks the object for a page of its own (`require_owned_page()`: a copy of the shared page, or the shared page itself for the last node that sees it) and maps that; a fetch from an entry without Execute maps the entry's frame with Execute; a physical object has no copy-on-write and maps its frame with what the region allows. A mapping given a permission by `protect_locked()` (an `mprotect` that widens) leaves its entries as they are, and the first access that needs the permission is what adds it, to that page only.

**Change**
```cpp
// mod.hpp, ProtectVisitor::on_terminal
auto const current = MmuFlagsConv<ArchMmuFlags>::from_arch(entry.arch_mmu_flags());
auto const kept = (current & ~MmuFlags::PermMask) | (current & m_flags & MmuFlags::PermMask);
entry = entry.with_protection(MmuFlagsConv<ArchMmuFlags>::to_arch(kept));
```
In `page_fault()`, an entry that is present is now stale for the access unless it lacks something the access needs (Write for a write, Execute for a fetch); if it does, the permission is added by `widen_present_locked()` and if it does not the fault is spurious. The old branch that called `protect()` on an entry whose permissions differed from the region's is gone: with a `protect()` that cannot widen, it had nothing left to do but loop. A page the cursor says is not the node's to write is mapped without Write even in a mapping that allows it, and the debug assertion that stood in for that case is gone.

Rejected: keeping `restrict()` under its own name and `protect()` as it was. That is the state of the previous commit, and it leaves the hazard one call away.

**Reference**
Linux `mprotect()` widens lazily in the same way: `change_protection()` never adds Write to a present PTE of a private mapping, and the write fault (`do_wp_page()`) decides whether the page can be written, copied or reused. Zircon's `VmMapping::ProtectLocked()` widens by unmapping ranges that gain a permission and faulting them back in. Its `ProtectOrUnmap()` is what the narrowing here corresponds to.

**Verification**
- `scripts/format_cpp.py --check` clean on all files; `clang-tidy` nothing on the changed lines (old naming and `virtual` warnings elsewhere in `vm_mapping`).
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts, and the shell's `vma_map`, `protect` and `unmap` checks run through the new code in each. `vmosnapshot.test` 142 checks ok on both images (127 before, 15 new); `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel.
- The new checks take real soft faults, on a thread attached to an address space of its own: a page made read-only stays so through a `protect()` to what the mapping already allows; a write to it is resolved by the fault path, which leaves it writable on the same frame with the bytes it had; `protect()` to read-only takes Write from every entry at once, a write to a mapping that no longer allows it is refused and the entry stays; `protect()` back to read-write leaves the entries read-only, and the first write to a page adds Write to that page only; an executable mapping loses Execute on `protect()` to read-only, does not get it back on `protect()` to executable, and gets it from an instruction fetch, with no Write.
- The checks can fail. Three mutants: `protect()` setting the permissions again fails four of 139; the fault path never widening fails two; the fault path not counting an instruction fetch as needing Execute fails one. A fourth, an execute-only widening that adds Write, could not be told from the original and was not counted: a mapping cannot have both Write and Execute, so the case does not arise, and the expression is kept as the plainer statement of what that path may do.
- Not verified: a fault on a page shared with another node, which needs a node that has one. That is the next commits, and the same fault path is what they will exercise. Also not verified: another CPU's TLB, as in the previous entry.

**Deliberately not done**
- `map_paged()` still asks for an owned page for every page, so mapping a node that shares pages copies them all at map time. It has to map by what the cursor says, and flush its batch when the permission changes; that comes with the first mapping of a shared node.
- No shootdown: `protect()` and the unmap in `remap_locked()` flush the local TLB only.
- No change to how a physical object is faulted: it still maps its whole window at creation, and a fault on an absent page is still refused.

**How it was found**
The user asked why a new operation had been written, and whether `protect` was not already the narrowing one.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is unmap then map the only way this design adds a permission, and what does that force the caller to have?
- Why does a `protect()` that widens in the middle of a range leave a shared page writable, and what does a narrowing `protect()` do instead?
- Why is the entry that lacks a needed permission not spurious, when an entry that has everything is?

## Title: A VmCowPages knows the objects over it, and a clone takes Write from all their mappings (2026-09-30)

**Files**
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `VmCowPages::Attachment`, `attach_locked()`, `detach_locked()`, `nr_attached_locked()`, `remove_write_locked()`; `create_clone_locked()` calls it when it puts a hidden node above a node.
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`, `kernel/mm/vm_object_paged.cpp` — a `VmCowPages::Attachment` member registered at construction and taken off in the destructor, `cow_pages()`, `remove_write_of_cow_range_locked()`.
- `kernel/commands/vmo_snapshot_test.cpp` — 19 more checks, 161 in all.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — 8c done; 8d renamed.

**Symptom**
Latent, cannot be triggered today: nothing but the test clones a node that has an object over it. Without this a clone would take Write from the mappings of one object over the pages and leave the mappings of another, a slice, writable over pages that are now shared, and a write through the slice's mapping would go to the page every node sees, with no fault and no error.

**Cause**
The mappings of a page belong to the objects over it, and this tree lets more than one object sit on one `VmCowPages`: a slice shares its parent's pages and keeps its own list of mappings. Zircon has one object per `VmCowPages`, so a single back pointer (`paged_ref_`) is enough there, and the clone reaches every mapping through it. Here the node has to know all its objects, and a walk of the node's own bookkeeping is not enough to find the mappings.

The list is a member of the object, an `Attachment`, and not a base class: the node's header names the object only by a forward declaration, and the node calls into the object in its `.cpp`, so the two headers do not include each other. The object registers in its constructor and takes itself off in its destructor, both under the tree's lock; the lock the walk holds is the lock each object is registered under, so the walk never sees an object half destroyed, and the reference to the pages is released after the destructor's body, outside the lock, as the last reference to a node must be.

An object shows a window of the pages, so the node passes each object the range in the pages' bytes and the object translates it into its own, clamps it to its window, and hands it to `VmObject::remove_write_locked()`.

**Change**
```cpp
auto VmCowPages::remove_write_locked(u64 offset, u64 len) -> void {
    for (auto &attachment: m_attachments) {
        attachment.m_owner->lock().assert_held();
        attachment.m_owner->remove_write_of_cow_range_locked(offset, len);
    }
}
```
and in `create_clone_locked()`, in the part that cannot fail and before the pages move: `remove_write_locked(0, m_size);`. Only when a hidden node is put above the node: a node that holds nothing in the window and hangs under a hidden node already has no page of its own there, so its entries are read-only or absent already.

Rejected: a count of writable mappings on the node, which would have needed hooks in four places and refused to clone what a writable mapping shows; and making a slice a `VmCowPages` of its own, as Zircon does, which is a redesign of how slices are built.

**Reference**
Zircon `VmCowPages::CloneNewHiddenParentLocked()`: `RangeChangeUpdateLocked(0, size_, RangeChangeOp::RemoveWrite)` before the pages move, reaching the object through `paged_ref_` (`vm_cow_pages.cc:1618`).

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the changed lines.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts. `vmosnapshot.test` 161 checks ok on both images (142 before, 19 new); `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel.
- The checks with real mappings: an object and two slices are over the same pages and the pages know all three, and one that dies is off the list; an operation on the pages' second page takes Write from that page in the object's mapping and in the slice's, each at its own offset, and from no other, and not from a slice that does not show it; the whole range takes it from every mapping of every object. End to end: an object is mapped writable, holds a pattern, and its pages are cloned: Write is off every entry on the frames it had; a write through the mapping is resolved, and the entry is writable on a frame of its own while the other pages are not; the object's page holds the bytes it had, copied; and after the object writes its page the clone still sees the old bytes in the page the object gave up.
- The checks can fail. Three mutants: reaching only the first object fails two of 151; not translating the offset fails one; the clone not taking Write off the mappings fails two of 161.
- Not verified: another CPU's translation of an entry that lost Write, as in the entries before.

**Deliberately not done**
- No `create_clone()` on the object yet: the clone is made on the pages, in the test. 8d gives it its own name and the pinned refusal.
- No registry for an object that is not paged: physical objects have no copy-on-write.
- No cost measurement: the walk is proportional to the objects over the pages, which is a slice or two.

**How it was found**
Designing 8c: reading how Zircon reaches the mappings, and finding that this tree shares a node between objects.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a slice need its own registration with the pages, and why could a single back pointer do in Zircon?
- Why is the range translated by each object, and what would a slice's mapping lose Write on if it were not?
- Why is the object's reference to the pages released outside the lock?

## Title: VmObjectPaged::create_clone() makes a copy-on-write clone of an object (2026-09-30)

**Files**
- `kernel/mm/include/ours/mm/vm_object_paged.hpp`, `kernel/mm/vm_object_paged.cpp` — `VmObjectPaged::create_clone(offset, size, out)`.
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `create_clone_locked()` takes the end of the window the caller shows; two ternaries I had written there became `if` statements.
- `kernel/commands/vmo_snapshot_test.cpp` — 25 more checks, 186 in all.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — 8d done.

**Symptom**
None: this is the interface the rest was built for. Until it a copy-on-write clone could only be made on the pages, in the test; nothing that owns an object could ask for one.

**Cause**
The object is what the rest of the kernel holds: it has the flags, the window into the pages and the mappings. Three things belong to it and not to the pages.

Pinning is the object's: `VmoFlags::Pinned` says its pages are not to move, and a page that is shared is a page that another node may take, so an object with pinned pages is refused, `BadState`, as Zircon refuses a clone while a page is pinned. The pages cannot see the flag.

The window is the object's: a slice shows part of the pages it shares with its parent, and a clone of a slice must not see the parent's bytes past the slice, however long it is asked to be. The object passes the end of its window to the pages (`create_clone_locked(..., end, ...)`), which clamp what the clone sees to it, and the clone reads zero past that. Without it a clone of a slice would read the next page of the parent.

And the new object is made outside the lock: its constructor registers with the pages under the tree's lock, which the clone holds. If it cannot be made the clone goes with the reference to its pages, and the object's pages are left as the clone put them, which is consistent: the hidden node is merged away when the clone that hung under it is gone.

**Change**
```cpp
auto VmObjectPaged::create_clone(u64 offset, u64 size, ustl::Arc<VmObjectPaged> *out) -> Status
    // page aligned, offset no further than the end, size at most MAX_SIZE: InvArgs
    // Pinned: BadState
    // under the lock: m_cow_pages->create_clone_locked(m_cow_offset + offset, size,
    //                                                  m_cow_offset + m_size, &pages)
    // outside it: VmoPagedCache::get(...)  // registers with the pages
```
The clone has this object's flags, is not resizable, and has no pages of its own until it writes.

Named after Zircon's `CreateClone()` and this design's `VmCowPages::create_clone_locked()`, and not `create_child_snapshot()`, on the user's review: Zircon's "snapshot" names three different guarantees (`SnapshotType::Full`, `Modified`, `OnWrite`), and a function called `snapshot` would not say which. `create_child_slice()` keeps its name, as Zircon's `CreateChildSlice()` does.

**Reference**
Zircon `VmObjectPaged::CreateClone()` with `SnapshotType::OnWrite`, the bidirectional path (`vm_object_paged.cc:732`), which calls `VmCowPages::CreateCloneLocked()`; `CreateChildSlice()` for the pair.

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the changed lines.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts. `vmosnapshot.test` 186 checks ok on both images (161 before, 25 new); `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel.
- Through the object's own interface: a clone reads what the object held, pages and the hole as zero; each side's writes after the clone do not reach the other; a second clone reads the object as it is then, and the first is unchanged; a clone longer than the object reads zero past its end and a write there is its own; a clone of a clone; a misaligned offset or size and an offset past the end are `InvArgs`; a Pinned object is `BadState`; the clone of a slice, asked to be longer than the slice, reads the slice's pages and zero past it, not the parent's next page, and writes through the object or the slice do not reach it.
- Behind a mapping: a read fault of a clone's page maps the frame the object holds, read-only, in a mapping that allows Write; a write to it is resolved onto another frame holding the bytes copied, and the object's page is as it was; a write to a page never mapped is mapped writable on a copy.
- The checks can fail. Four mutants: not passing the end of the window fails one; not moving the offset into the pages' bytes fails two; the Pinned refusal removed fails one; a shared page mapped with Write fails two of 186. (Two of the first attempts did not match the text they meant to change and reported the unmutated build; they were redone.)
- Not verified: another CPU's TLB (a clone leaves the old frame read-only in a stale translation there until it is flushed); the failure of the object's construction after the clone was made, for want of a way to make it fail on demand.

**Deliberately not done**
- No option or syscall: `VmoDispatcher::create_child()` still answers `NotSupported` for it, and there is no `ou::vmo_create_child` for a program to ask; that is step 10.
- `map_paged()` still asks for an owned page for every page, so a mapping made with `Commit` copies every shared page at map time; the fault path is right and is what a lazy mapping uses. It has to map by what the cursor says, and flush its batch when the permission changes.
- No `SnapshotType`: one guarantee, the one implemented.

**How it was found**
None: a step of the design. The name is the user's review.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the refusal of a Pinned object in the object and not in the pages?
- What does a clone of a slice read past the slice, and what would it read if the end of the window were not passed to the pages?
- Why is the new object made after the lock is released?

## Title: map_paged() maps what the cursor says, and copies nothing (2026-10-01)

**Files**
- `kernel/mm/vm_mapping.cpp` — `map_paged()` asks for `require_page(false, ...)` and maps each page by its `writable`; `MappingCoalescer::set_flags()`.
- `kernel/commands/vmo_snapshot_test.cpp` — 8 more checks, 194 in all.
- `.agents/tasks/vmo-snapshot-child/DESIGN.md` — step 9 done; a new finding about `Commit`, in the open questions.

**Symptom**
Latent, cannot be triggered by a program yet: nothing maps a clone but the tests. `map_paged()` asked for an owned page for every page of the window, so filling in the page tables of a mapping of a clone copied every shared page it covered, at the moment of mapping, and mapped each copy writable. That is the copying a clone exists to put off, done up front, and for a region of a large file it is all of it.

**Cause**
`map_paged()` was written when a page was either the node's own or did not exist, so "the page for this offset" had one answer and it was writable. A node that can see pages of its ancestors has three: its own, writable; one of an ancestor's, on loan and read-only; and the shared zero page, read-only. Populating a mapping has no writer, so it is a read, and the existing interface for the choice is `Cursor::require_page(will_write, ...)`, which the fault path already uses; the user pointed out that it exists, where I had said to call `require_read_page()` directly. For a node with no ancestor a read of a hole is a zeroed page of its own, which is what it was.

What each page may have is what the region allows, less Write when the cursor says the page is not the node's to write, which is how the fault path maps it. The batch that installs the entries takes one set of permissions, so a run of pages whose permissions change is committed at the change and continues with the new ones: `MappingCoalescer::set_flags()`, allowed only when the batch is empty.

**Change**
```cpp
auto result = cursor->require_page(false, 1, &page_request);
auto page_mmuf = mmuf;
if (!result->writable) { page_mmuf &= ~MmuFlags::Writable; }
if (page_mmuf != batch_mmuf) { coalescer.commit(); coalescer.set_flags(page_mmuf); batch_mmuf = page_mmuf; }
coalescer.append(frame_to_phys(result->page));
```

**Reference**
Zircon `VmMapping::MapRange()`, which maps the pages that exist without committing any, read-only where the page belongs to an ancestor (the same rule as `page_fault()`); `LookupCursor::RequireReadPage()` with `writable = false` for a page from a parent.

**Verification**
- `scripts/format_cpp.py --check` clean; `clang-tidy` nothing on the changed lines.
- `boot_check.py --out out/vmo-snapshot`: both images PASS, the same counts, and the shell's mapping checks run through the changed function. `vmosnapshot.test` 194 checks ok on both images (186 before, 8 new); `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 on rel.
- The new checks map a clone four pages long of an object of three, after the clone has written its page 1, and fill its page tables: the page it only sees is on the object's frame, read-only, in a mapping that allows Write; the page it wrote is writable, in the middle of the run; a hole of the object and a page past its end are the shared zero page, read-only; and the clone still holds exactly one page, so mapping copied nothing.
- The checks can fail. Three mutants: asking for an owned page again fails three of 194; not splitting the batch at a change fails two; a shared page mapped with Write fails two.
- Not verified: anything with another CPU.

**Deliberately not done**
- `VmMapOption::Commit` still commits the object first (`VmArea::map_with_vmo()` calls `commit_range()` and then `map()`), and `commit_range()` on a clone copies every page of the range, since an owned page is what it asks for. So populating a mapping of a clone through `map_with_vmo()` still copies, and the test fills the page tables through `VmMapping::map()` directly. What populate and commit should mean on a node with ancestors is a question for the user and is recorded in the design; Zircon's populate maps what exists and leaves commit to the explicit operation.

**How it was found**
The user's question whether `require_page()` was not the interface; and a first version of the test that mapped with `Commit` and found the page tables full of copies, which led to `commit_range()`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is populating a mapping a read and not a write, and what does it return for a page the node only sees?
- Why must the batch be committed when the permissions change?
- Why did mapping with `Commit` still copy, after this change?
