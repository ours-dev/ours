# mm outstanding repairs

## 1. A fork of a shared page zero-fills the frame it is about to overwrite

**Severity**
Low. Correct and only a cost: a copy-on-write fork writes each frame twice, and it is a cost per first write to a shared page, which is the path a program started from a shared file takes for every data page it touches.

**Symptom**
None observable. `VmCowPages::cow_page()` allocates the copy with `alloc_pages()`, which asks the page allocator for `Gaf::Zero`, so `PmZone::allocate()` fills the whole frame with zeros; the next line copies the source over all of it.

**Cause**
`alloc_pages()` adds `Gaf::Zero` unconditionally, on purpose: every VMO byte nobody has supplied reads as zero, and a frame from the buddy allocator holds what its previous owner left. That guarantee is right for a hole and wasted for a page that is about to be overwritten in full. Zircon's `AllocateCopyPage()` allocates without the fill for the same reason.

**Change**
Let `alloc_pages()` take whether the caller will overwrite the frame, and leave `Gaf::Zero` off when it will. Before doing it, measure, as CLAUDE.md asks of a performance item: time `cow_page()` for a fork with and without the fill (per-page cycles on the dev image), and say how much of a launch of a program with N data pages it is. Ruled out for now: zero-filling lazily, which would have to track which frames were filled.

**Reference**
Zircon `VmCowPages::AllocateCopyPage()` (`vm_cow_pages.cc`), which takes a page from the PMM without asking for zeroing and then copies.

**Acceptance**
`cow_page()` copies into a frame that was not zeroed first; `vmosnapshot.test` still passes, including the checks that a copied page holds exactly the source's bytes; the measurement is recorded in the change entry that removes this item.

## 2. A leaf's death walks every page of its window

**Severity**
Low. Correct and only a cost, paid once per leaf that dies: `release_viewed_pages_locked()` asks `find_page()` for every offset in the window the leaf saw, whether or not anything is there, under the tree's lock.

**Symptom**
None observable. A window of N pages costs N walks up the chain of hidden nodes, so the death of a process that cloned a large segment holds the lock of every other process of that program for that long. A window of a 100 MB boot image is about 25,000 lookups.

**Cause**
The loop is written per offset because it had to be right first: each offset needs the nearest page that the leaf saw, and a page of its own at an offset shadows what is above. Most offsets of a large window hold nothing, and the loop still visits them.

**Change**
Walk the pages that exist instead of the offsets that might: for each ancestor, iterate its store between the limits of the window (`VmPageList::next_slot()` is there for it), skipping an offset a nearer node or the leaf itself holds. Measure first, as CLAUDE.md asks of a performance item: the time of a death with N pages of window, and how much of it is lock hold time, with the benchmark planned for decision 3 of the design.

**Reference**
Zircon `ReleaseOwnedPagesRangeLocked()` (`vm_cow_pages.cc:2842`), which walks the page list of each node in the range, not the offsets.

**Acceptance**
A death touches only the pages in the window; `vmosnapshot.test` still passes, including the count checks of the death scenarios; the measurement is recorded in the change entry that removes this item.
