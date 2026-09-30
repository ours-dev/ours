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
