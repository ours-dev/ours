# mm-pin outstanding repairs

## 1. `CommitOptions::Pin` and `commit_range_pinned()` say they pin and do not

**Severity**
Low: a misleading name on a path nothing depends on for a pin, since the real one is `pin_range()`.

**Symptom**
Latent, cannot be triggered today. A caller that reads `commit_range_pinned()` and trusts it to hold the pages would get none held.

**Cause**
`VmObject::commit_range_pinned()` passes `CommitOptions::Pin` to `commit_range()`, and `VmObjectPaged::commit_range_internal()` does nothing with the flag beyond refusing an empty range. The pages of a `VmoFlags::Pinned` object are pinned another way: `VmCowPages::file_page_locked()` files each on the pinned queue as it is put in the page list. The option's comment ("the pages mapped will not be swapped out") describes an intention, and `pin_range()` is the first thing that counts and undoes a pin. The option cannot simply be wired to `pin_range()`: a `VmoFlags::Pinned` object commits with it at creation, and those pages would then hold a pin nothing ever undoes, which `~VmCowPages` asserts against.

**Change**
Remove the option and the helper, or make the option mean what it says and have the creation of a pinned object stop passing it. Leave the decision until the BTI work is done and nothing else is found to call them. Ask before deleting: they may be scaffolding.

**Reference**
Zircon `VmObject::CommitRangePinned()` (`kernel/vm/include/vm/vm_object.h:320`), which commits and pins in one call (`VmCowPages::PinRangeLocked()`, `vm_cow_pages.cc:4022`) and is undone by `VmCowPages::UnpinLocked()` (`:5269`): the shape `pin_range()` has.

**Acceptance**
No caller of `commit_range_pinned()`, or a test showing that it holds the pages.

## 2. `decommit` and a resize will have to respect a pin

**Severity**
Medium: not reached today, and a bug at once on the day either is built.

**Symptom**
Latent, cannot be triggered today: `VmObjectPaged::decommit()` answers `Unimplemented`, and a VMO cannot be resized (`Resizable` is NotSupported).

**Cause**
A pinned page must stay in its frame while a device may write to it. Nothing that removes pages from a node honours that except the two routes `pin_range()` guards (a clone, `take_pages()`) and the reclaimer, which skips pages that are not on a reclaimable queue. A decommit or a truncating resize written later would free a pinned page unless it asks `pinned_count()` first.

**Change**
When either is built: refuse with BadState if any page of the range holds a pin, as `take_pages_locked()` does, and add the case to `vmopin.test`.

**Reference**
Zircon `VmCowPages::DecommitRange()` (`kernel/vm/vm_cow_pages.cc:4086`) and `Resize()` (`:5477`), which refuse a range with pinned pages through `AnyPagesPinnedLocked()`.

**Acceptance**
`vmopin.test` has a decommit and a resize over a pinned range answering BadState.

## 3. A pin on a VMO with a page source is untested

**Severity**
Low: pager-backed VMOs are not pinned by any caller today.

**Symptom**
Latent, cannot be triggered today. `pin_range()` is meant to work on a VMO made by `VmCowPages::with_source()`: a page the source has not supplied makes `pin_range_locked()` answer ShouldWait, and the loop in `VmObjectPaged::pin_range()` waits and goes on from the page it stopped at. That loop has not run. What a pin leaves behind is covered: a page of such a VMO is written once it has been pinned, and not reclaimable after the unpin (`pagereclaim.test`, `docs/changes/261103/mm.md`).

**Cause**
`vmopin.test` uses an anonymous VMO. A source-backed one needs the provider machinery of `pagesource.test` and `pagereclaim.test`.

**Change**
A case in `vmopin.test` that pins a source-backed range with a provider that supplies late, and shows the pin completes after the wait.

**Reference**
None, this project's own design.

**Acceptance**
That case passes, and removing the wait-and-go-on in `VmObjectPaged::pin_range()` fails it.

## 4. A read-only pin of a page-source page makes it unreclaimable for good

**Severity**
Low: a page that stays in memory when it could have been given back, and nothing pins a pager-backed VMO today.

**Symptom**
Latent, cannot be triggered today. A device that only reads a page of a pager-backed VMO holds a pin on it. When the pin goes, the page is filed as written (anonymous) and stays so, although nothing changed it: the reclaimer never offers it for eviction again, and the page source cannot be asked for it again.

**Cause**
`pin_range()` cannot tell a read pin from a write pin: it takes each page through `require_owned_page()`, which marks a page of a page source written, as a write through a mapping does. `unpin_range_locked()` keeps that mark (`docs/changes/261103/mm.md`), since a pin that was for writing is the case that must not lose data. A page has no clean/dirty bit of its own, only the queue it is on, and there is no write-back that would turn a written page clean again (`.agents/tasks/page-reclaim/DESIGN.md:27` puts dirty pager pages and their write-back off for later).

**Change**
Have `BtiDispatcher::pin()` pass whether any permission can write to `pin_range()`, and let a read-only pin take the page through `require_read_page()` and file it back as it was. That needs the page's pre-pin kind kept somewhere while it sits on the pinned queue, or a per-page dirty bit.

**Reference**
Zircon `VmCowPages::MoveToNotPinnedLocked()` (`kernel/vm/vm_cow_pages.cc:4761`): a page of a pager-backed VMO goes back to the reclaim queue if `is_page_clean()`, and to the dirty queue otherwise, so the page itself says what it was.

**Acceptance**
`pagereclaim.test` has a read-only pin and unpin of a page-source page that leaves the page reclaimable, and a write pin that does not.
