# arch-paging outstanding repairs

## 1. The page table takes map options and ignores them

**Severity**
Low: no caller asks for anything but ErrorIfExisting, which is what happens anyway.

**Symptom**
Latent, cannot be triggered today. `ArchVmAspace::map()` and `map_bulk()` take `ArchMapOptions` (`SkipIfExisting`, `OverwriteIfExisting`, `ErrorIfExisting`, `TryLargePage`, `TryMerge`), and `PageTable::map_pages()` and `map_pages_bulk()` drop them (`(void)options`): an entry already there is always an error, no large page is ever tried, nothing is merged. A caller asking to skip existing entries gets AlreadyExists.

**Cause**
The options were declared with the interface and never implemented in the paging engine (`Engine::map()` takes none). Until 2026-10-24 mm's `MappingCoalescer` passed a hard-coded `TryLargePage` and its callers' choice was dropped one level higher, so nobody saw that the engine dropped it too. Large leaves are now mm's decision (`VmMapping::map_block_locked()`, `.agents/tasks/large-pages/`), so `TryLargePage` and `TryMerge` have no user left.

**Change**
Either implement the existing-entry actions in the engine (skip, error, overwrite) and drop the two large-page bits, or drop the parameter until a caller needs one. A caller that will need skip is a range map over a mapping that may already be partly mapped, the case Zircon's `ignore_existing` is for.

**Reference**
Zircon `ArchVmAspace::Map()` and `MapContiguous()` with `ExistingEntryAction` (`Skip`, `Error`, `Upgrade`), and `X86PageTableBase::MapPages()`.

**Acceptance**
Either a host test in `x86-paging-smoke` mapping over an existing entry with each action and seeing it honoured, or the parameter gone from `ArchVmAspace` and `PageTable`.
