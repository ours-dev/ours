# mm-pin changes

## A range of a VMO can be pinned, counted and undone (2026-10-30)

**Files**
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `VmmBody::pinned_count` gets its comment, and `PageFrame` gets `pinned_count()`, `increase_pinned()` and `decrease_pinned()`.
- `kernel/mm/vm_cow_pages.cpp`, `include/ours/mm/vm_cow_pages.hpp` — `pin_range_locked()`, `unpin_range_locked()`, `nr_pinned_pages_locked()` and the node's count of pinned pages; the refusals in `create_clone_locked()` and `take_pages_locked()`; two assertions, in `~VmCowPages()` and `free_page()`.
- `kernel/mm/vm_object_paged.cpp`, `include/ours/mm/vm_object_paged.hpp`, `include/ours/mm/vm_object.hpp` — `VmObject::pin_range()` and `unpin_range()`, NotSupported by default, and the paged object's, with the page source wait.
- `kernel/commands/vmo_pin_test.cpp`, `BUILD.gn` — the console command `vmopin.test`.
- `docs/repairs/261030/mm-pin.md`, `.agents/tasks/driver-capabilities/DESIGN.md` — what this step leaves, and what it did.

**Symptom**
None; this is step 4 of `.agents/tasks/driver-capabilities/PLAN.md`. Before it nothing could hold a page of a VMO in its frame and later let it go. `commit_range()` with `CommitOptions::Pin` did nothing beyond refusing an empty range, and a `VmoFlags::Pinned` object pinned every page for its whole life, with no way to undo it. The Bus Transaction Initiator of the next steps has to pin a range for as long as a device may write to it, and unpin it when the driver says the device is done.

**Cause**
The objects:
- a page of a VMO (`VmPage`, a `PageFrame` in the VMM role): its descriptor already had a one-byte `pinned_count` that nothing read or wrote;
- the node (`VmCowPages`): the VMO's pages and the one lock that guards them, and the clone tree they hang in;
- the page queues: where the reclaimer looks for pages to give back;
- the reclaimer: takes a page off a queue, then asks the node, under the node's lock, to free it.

The events of `pin_range(P, 2P)` on a fresh lazy VMO of four pages:
1. `VmObjectPaged::pin_range()` takes the node's lock and calls `pin_range_locked()` for pages 1 and 2.
2. For each page the cursor commits it as the node's own, or copies it from an ancestor if the node only saw it through one.
3. The page's count goes from 0 to 1, the page moves to the pinned queue, and the node's count of pinned pages goes up.
4. If the node's page source has to supply a page first, the call answers ShouldWait with how many pages it pinned; the object lets go of the lock, waits, and goes on from there. The pages already pinned stay pinned.
5. If a later page fails (the 256th pin of a page, say), the object undoes the pins it made and answers the error: the range is pinned whole or not at all.
6. `unpin_range()` takes one pin off each page. When a page's last one goes, it is filed again as its node files a new page, anonymous for an anonymous node, reclaimable for a pager's.

Why the pin reaches the reclaimer without the reclaimer being changed: `reclaim_page()` and `dedup_zero_page()` already ask the page's queue kind under the node's lock and let a page go that is not on the queue they look for. A pinned page is on the pinned queue, so both skip it. That is the existing handshake for a page written since the reclaimer chose it, and the pin uses it as is.

The worst interleaving, and why it is safe. Everything that changes a pin, a page's queue or the node's page list takes the node's lock, and the pin count and the node's count are written only under it. The race that matters is the reclaimer's: it chooses a page under the queue's spin lock and lets go of it, and only then takes the node's lock. A pin that gets the node's lock in between moves the page to the pinned queue; the reclaimer then finds it is not reclaimable and keeps it. In the other order the reclaimer frees the page first and the pin commits a fresh one at the same offset. Either way no pinned page is freed. The lock order is the existing one, the node's lock and then a queue's.

What a pin has to forbid, and where it is held. A clone moves a node's pages under a hidden node, and the pages become shared, so `create_clone_locked()` is BadState when the node holds any pinned page; it asks the whole node and not only the window, because the hidden node takes everything the node has. `take_pages_locked()` removes pages from the node and is BadState over a pinned one. A decommit and a resize would too, and neither exists yet (`docs/repairs/261030/mm-pin.md`, item 2). The 255 limit is the byte's: past it a pin is NoResource, not a wrap to none, which would let a page loose while pinned.

**Change**
The core, in the node:

```cpp
for (usize at = 0; at < size; at += PAGE_SIZE) {
    auto result = cursor->require_owned_page(1, request);
    if (!result) {
        return result.unwrap_err();
    }
    auto *const page = result->page;
    if (!page->increase_pinned()) {
        return Status::NoResource;
    }
    if (page->pinned_count() == 1) {
        queues_of(page).set_pinned(page, this, PgOff((offset + at) >> PAGE_SHIFT));
        m_nr_pinned_pages += 1;
    }
    *nr_pinned += 1;
}
```

Rejected: giving the existing `CommitOptions::Pin` this meaning. A `VmoFlags::Pinned` object commits with it at creation, so every page of one would hold a pin nothing undoes, and `~VmCowPages` asserts that none is left. The option stays as it was (`docs/repairs/261030/mm-pin.md`, item 1). Also rejected: an unmatched unpin as an assertion, which is what the reference does; it is BadState here, with nothing undone, because the caller is a system call that must not be able to panic the kernel.

**Reference**
Zircon `VmObject::CommitRangePinned()` (`kernel/vm/include/vm/vm_object.h:320`), `VmCowPages::PinRangeLocked()` (`kernel/vm/vm_cow_pages.cc:4022`) and `UnpinLocked()` (`:5269`). Its decommit (`DecommitRange()`, `:4086`) and resize (`Resize()`, `:5477`) refuse a range with pinned pages through `AnyPagesPinnedLocked()`; ours has neither operation yet.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS (151 passed).
- `kshell_run.py`: `vmopin.test: ok -- 28 checks`, and the tests of the paths the change touches unchanged: `pagereclaim.test` 18, `pager.test` 23, `pagesource.test` 76, `vmosnapshot.test` 241, `pagequeues.test` 12, `mempressure.test` 15, `largepage.test` 28, `vmaphys.test` 8, `physvmo.test` 6, `resource.test` 60.
- A mutation, undone afterwards: with the queue move, the clone refusal and the `take_pages()` refusal each taken out, `vmopin.test` failed on exactly the checks for them (the pinned queue, the clone, `take_pages()`, the zero dedup keeping a pinned page), and on the checks that follow a clone that should not have been made.
- The first run of `vmopin.test` had one failure, in the test: it passed page 1's page with the index of page 0 to `dedup_zero_page()`, which answers Gone for that.
- clang-tidy on `vm_cow_pages.cpp`, `vm_object_paged.cpp` and `vmo_pin_test.cpp`: three diagnostics, all on lines this change did not write. clang-format: clean.
- Not measured: a pin on a VMO with a page source (`docs/repairs/261030/mm-pin.md`, item 3), and contention between a pin and the reclaimer on several CPUs; the argument above is the whole of the second.

**Deliberately not done**
Anything that calls it: the BTI is step 5. A pin of a physical VMO, which has no pages of the kernel's to hold and is the BTI's to answer. A decommit and a resize that honour a pin, which do not exist.

**How it was found**
None, a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A reclaimer chose a page just before a pin. Which two orders can the pin and the reclaimer's lock go in, and what does each leave?
2. Why does a clone have to be refused for the whole node when only one page is pinned?
3. Why can the existing `CommitOptions::Pin` not simply be made to call `pin_range()`?
