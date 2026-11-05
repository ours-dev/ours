# vmm-naming: delivered interface renames

**Date**: 2026-11-05

## Delivered

- `6427e51d` names asynchronous page source requests request_pages.
- `7fcdaaca` nests the existing cause bitmask as VmFault::Cause and keeps VmFault::cause.
- `203bf951` names the COW and paged VMO factories lookup_cursor_locked and their result Cursor::PageLookup.
- `211eb6e8` names VMO-backed mapping map_vmo across all kernel and benchmark callers.
- `e741fb30` nests UnmapScope in VmArea, with MappingsOnly and IncludeSubvmas, and updates the dispatcher and syscall conversion.
- `7b0791c3` names the common range helper resolve_range_locked.
- The task plan records the user's choices for all twelve entries, including the previously delivered single MapOptions bitmask.

The final UnmapScope variants are Mapping and Subarea, selected by the user in the follow-up to this task.

## Verification

Each increment passed LLVM 23 clang-format and clang-tidy on every affected C++ file, both development image boots and all 163 host tests. Existing pager, user-copy, COW, pin, contiguous, large-page, mapping, undo and placement commands exercised the renamed entries. The final placement, seal and large-page commands passed. Symbol searches found no remaining approved old names in the kernel, system or benchmark sources. All flag values, mapping policies and locking requirements were preserved.

## Deviations from the design

None.

## Found along the way

None.

## Not done

None.
