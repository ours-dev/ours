# mm changes

## Title: unpinning a page of a page source keeps it written, so the reclaimer cannot evict what a device wrote (2026-11-03)

**Files**
- `kernel/mm/vm_cow_pages.cpp` — `unpin_range_locked()` files the page with `Fill::Written`; `file_page_locked()` sends a `Written` page of a node with a page source to the anonymous queue
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — the `Fill::Written` enumerator, and the comments of `file_page_locked()` and `unpin_range_locked()`
- `kernel/commands/page_reclaim_test.cpp` — `check_unpin_keeps_the_write()`, three new checks in `pagereclaim.test`
- `docs/repairs/261030/mm-pin.md` — item 3 no longer says the page is reclaimable after the unpin; item 4 records the cost of this change
- `.agents/skills/kernel-design/references/mm.md` — "a pin counts as a write" next to the reclaim decisions

**Symptom**
A page of a pager-backed VMO that a device wrote while it was pinned, or that a program wrote before the pin, is evicted under memory pressure after the unpin. The next read fetches the page source's old content, so the device's data and the program's stores are gone. The path from user space is `pager_create_vmo`, supply page 0, `vma_map` it read-write, `bti_pin` it with write permission, write, `pmt_unpin`, then memory pressure; it needs a BTI, so it is a driver's path, not an unprivileged program's. `pagereclaim.test` now reproduces it without the syscalls, by calling `pin_range()` and `unpin_range()` directly.

**Cause**
**The "this page was written" fact lived in one place, the queue the page is on, and a second fact took that place.** A page of a node with a page source is on the Reclaimable queue while it is clean, and `file_dirty_locked()` moves it to the Anonymous queue when it is written. There is no per-page dirty bit. `pin_range_locked()` takes every page through `require_owned_page()` because a device may write it, and that call marks the page written (Reclaimable to Anonymous). The pin then calls `set_pinned()`, which moves the page to the Pinned queue, and the Anonymous mark is gone: from then on the page's queue says only "pinned".

At the last unpin, `file_page_locked(page, index, Fill::Content)` asked the question it asks for a page that has just arrived: how does a page of this node normally come in? For a node with a page source the answer is "as the source's content", so the page went to Reclaimable. That was wrong for a page that had been handed to a writer.

Two things then make the loss silent. First, the page table entry stays writable: the write fault that marked the page written returned `.writable = true`, and nothing takes Write away at the unpin, so a later store does not fault and the node is never told again. Second, `reclaim_page()` checks only the queue kind under the owner's lock; it does not look at a hardware dirty bit, so it evicts the page as clean. The invariant stated at `Cursor::writable_here()` ("Reclaimable means clean, and mapped read-only everywhere") is broken by the unpin.

The general lesson: a fact kept only as a side effect of another fact (here, written as a side effect of the queue) is lost the moment the other fact changes (here, the page leaves for the pinned queue). The pinned queue is a state that does not remember where the page came from.

**Change**
The unpin files the page as written, not as the source's content. `Fill` gets a third value for it.

```cpp
// before: vm_cow_pages.cpp, unpin_range_locked()
file_page_locked(page, index, Fill::Content);

// before: file_page_locked()
} else if (m_page_source) {
    queues.set_reclaimable(page, this, index);
}

// after
file_page_locked(page, index, Fill::Written);

} else if (m_page_source && Fill::Written != fill) {
    queues.set_reclaimable(page, this, index);
}
```

For a pinned node (`Flags::Pinned`) the first branch still files the page as pinned, and for a node without a page source `Written` falls through to the anonymous queue as `Content` did, so only a page of a node with a page source behaves differently.

Rejected: refusing to pin a VMO that has a page source. It would close the bug in one line, but `docs/repairs/261030/mm-pin.md` item 3 says `pin_range()` is meant to work on such a VMO. Also rejected for now: a per-page clean/dirty bit, and having `pin_range()` carry the permission so a read-only pin keeps the page clean. Both are larger changes to the page and to the pin interface; the cost of not doing them is recorded as item 4 of the same repair file.

**Reference**
Zircon `VmCowPages::MoveToNotPinnedLocked()` (`kernel/vm/vm_cow_pages.cc:4761`): for a user-pager VMO it asks `is_page_clean(page)` and sends a clean page back to the reclaim queue, any other to the dirty queue, so the page itself remembers what it was. This kernel has no such bit, so the unpin cannot tell, and the safe answer is "written". `PinRangeLocked()` (`:4022`) does not change a page's dirty state, which is the part this kernel does differently: its pin has already marked the page written.

**Verification**
- Built and booted in a clean worktree (`.worktrees/verify-h1-pin`, HEAD `49899f75` plus these three C++ files), because the main checkout's build was broken by another session's uncommitted edit of `system/public/ours/types.hpp` (`PhysAddr` and `VirtAddr` as `enum class`), which I did not touch.
- `python3 .agents/skills/verify/scripts/boot_check.py`: build ok in 13.2 s; dbg PASS, shell finished at 5.6 s, 46 passed, 5 skipped, 0 failed; rel PASS, shell finished at 4.9 s, 44 passed, 8 skipped, 0 failed, 7/7 required; host-tests PASS, 161 passed, 0 failed.
- `python3 scripts/kshell_run.py --out out/bringup.x86 pagereclaim.test vmopin.test`: `pagereclaim.test: ok -- 21 checks` (18 before, 3 new), `vmopin.test: ok -- 28 checks`.
- Fail-first: with the unpin line put back to `Fill::Content` and everything else in place, `pagereclaim.test: FAILED -- 2 of 21 checks`, the two checks "a page unpinned is written, not reclaimable" and "reclaim(100) evicts the page never pinned, and keeps what the device wrote".
- `scripts/format_cpp.py --check` is clean on the three files. `clang-tidy -p out/bringup.x86` on `vm_cow_pages.cpp` and `page_reclaim_test.cpp` reports two warnings in `vm_cow_pages.hpp` (line 496, `modernize-use-override`; line 858, `modernize-use-nodiscard`), neither on a line this change touched.
- Not verified: the user-space path (`pager_create_vmo` to `bti_pin`) end to end, since no program drives it yet; the test calls `VmObjectPaged::pin_range()` and `unpin_range()` directly. The `MEM=3072`, SMP and NUMA boots were not run, as nothing here depends on them.

**Deliberately not done**
- A read-only pin still makes the page written for good, because `pin_range()` cannot tell the two kinds of pin apart. That loses reclaimability, not data, and is item 4 of `docs/repairs/261030/mm-pin.md`.
- No change to the page table entry at unpin. A writable entry on a written (anonymous) page is correct; what was wrong was the queue.
- The case in item 3 of the same file (a pin that has to wait for a late supply) stays untested.

**How it was found**
By the independent review in `.agents/tasks/review-last-three-days/REVIEW.md` (H1), which traced `pin_range_locked()`, `file_dirty_locked()` and `reclaim_page()` by reading. Three separate checkers re-derived it from the code, each from a different angle (the code path, the entry point and its preconditions, and the design records). None ran it; this entry's test is the first run.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does a pin already count as a write in this kernel, and why does that make the unpin the place where the mark has to be kept?
- Why must a Reclaimable page be mapped read-only, and what did the unpin leave behind that broke this?
- Zircon asks the page whether it is clean at the unpin and this kernel does not: what does this kernel do instead, and what does it cost?
