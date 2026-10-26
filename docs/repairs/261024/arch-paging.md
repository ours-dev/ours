# arch-paging outstanding repairs

## 1. The page table refuses OverwriteIfExisting

**Severity**
Low: no caller asks for it; the engine answers InvArgs, and the kernel asserts it is never asked.

**Symptom**
Latent, cannot be triggered today. Error and Skip, and Undo and Keep, are honoured since 2026-10-25 (`docs/changes/261025/arch-paging.md`). A map with `OverwriteIfExisting` is refused with InvArgs by `arch::paging::Paging::map()`.

**Cause**
Replacing an entry is the one action that changes a translation a CPU may have cached. It needs three things the other actions do not:

- a TLB invalidation of every entry it replaced, before the map returns;
- a split of a large leaf that the range covers only in part;
- a rule for a large leaf the range would map over a table of small entries.

The user chose on 2026-10-25 to implement it rather than drop it.

**Change**
In `map_one_from()`, replace a present leaf of the size the step would map. Split a larger leaf first, with its deposit; without deposits that is InvArgs, as for unmap. Map small entries under an existing table rather than replacing the table with a leaf. Report that something was replaced, so that `ArchVmAspace::map()` invalidates the range before it returns. The caller frees a replaced frame only after the map returns.

**Reference**
Zircon `ArchVmAspaceInterface::ExistingEntryAction::Upgrade`, and its x86 `X86PageTableBase::AddMapping()`.

**Acceptance**
`x86-paging-smoke` checks:

- a page replaced with another frame;
- a page given Write;
- a page inside a 2 MiB leaf replaced through a split;
- a 2 MiB range over a table of small entries replaced entry by entry.

And a kernel path, or a kshell check, sees the TLB invalidated: the new frame is read after the map.
