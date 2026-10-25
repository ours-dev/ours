# mm outstanding repairs

## 1. A zero marker put by hand over an ancestor's page leaves the page's viewer count high

**Severity**
Low: only tests call `add_zero_marker_locked()` on a node under a hidden one; nothing in the kernel does.

**Symptom**
Latent, cannot be triggered today. `VmCowPages::add_zero_marker_locked()` on a clone, at an offset where a hidden ancestor holds a page, hides the page from the clone without counting the clone out of the page's viewers. The count stays one too high, and when the hidden node is merged into its last child, `DEBUG_ASSERT(survivor.is_hidden() || 0 == page->share_count())` fires ("a page with another viewer in a hidden node with one child"). Found on 2026-10-25, when the first version of the take-from-clone check in `vmosnapshot.test` put such a marker and the kernel panicked at the end of the check.

**Cause**
A page in a hidden node counts how many leaves below it see it (`share_count()`). Everything that stops a leaf seeing one keeps the count: a write's copy (`cow_page()`), a leaf's death (`release_viewed_pages_locked()`), and `take_pages_locked()`, which only leaves a marker where the leaf already holds a page of its own, a copy made by `cow_page()`. `add_zero_marker_locked()` inserts a marker wherever it is told, and does not look up.

**Change**
Either drop the share of the page the marker hides (look it up with `shows_ancestor_page_locked()`'s walk, and `drop_share()` it, freeing it if this was the last viewer, as `release_viewed_pages_locked()` does), or refuse such an offset with a `DEBUG_ASSERT`, since no kernel path needs it. The second is enough until one does.

**Reference**
Zircon keeps the count the same way on every path that hides parent content (`VmCowPages::DecrementCowContentShareCount()`, called from `SupplyPagesLocked()`'s `handle_add_page_result` when a slot that showed a hidden ancestor's page is filled).

**Acceptance**
A `vmosnapshot.test` check that puts a marker on a clone over its hidden parent's page, destroys the clone's sibling, and finds the merge consistent (or, with the assert, that the debug build refuses the call).
