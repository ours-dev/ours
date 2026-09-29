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
