# vmm-naming: finish the approved VMM naming list

**Date**: 2026-11-05
**Status**: Accepted

## Request

Apply the names selected from the twelve-item VMM list. Use Rust naming conventions with Zircon as a secondary vocabulary reference. Treat the user's rejected names and retained spellings as settled decisions.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Page source request | get_pages, request_pages | request_pages | None | request_pages, confirmed earlier |
| Mapping options | Two enums, one bitmask | One bitmask | None | MapOptions with Lazy, Resident, Commit and Sealed, already delivered |
| Unmap child policy | Existing name, nested scope | Nested scope | None | VmArea::UnmapScope with MappingsOnly and IncludeSubvmas, 2026-11-05 |
| Placement resolution | create_subaom_preamble, resolve_range_locked | resolve_range_locked | None | Accepted, 2026-11-05 |
| Address selection | alloc_spot, find_free_base | Retain allocation vocabulary | None | Keep alloc_spot and find_spot, 2026-11-05 |
| Wait token | MultiPageRequest, PageWait | Retain selected name | None | Keep MultiPageRequest, 2026-11-05 |
| Fault cause | VmfCause, nested Cause | VmFault::Cause | None | VmFault::Cause and VmFault::cause, 2026-11-05 |
| Lookup cursor | make_cursor, lookup_cursor_locked | lookup_cursor_locked | None | Accepted, 2026-11-05 |
| Lookup result | RequireResult, PageLookup | Cursor::PageLookup | None | Accepted, 2026-11-05 |
| Range notification | Current name, proposed alternative | Retain selected name | None | Keep range_change_locked_object, 2026-11-05 |
| VMO mapping and child area | map_with_vmo, map_vmo; subvma, subarea | Apply selected spellings | None | map_vmo and create_subvma, 2026-11-05 |
| Sparse page transfer | VmPageSpliceList, PageSplice | Retain selected name | None | Keep VmPageSpliceList, 2026-11-05 |

## Steps

1. Complete the previously approved PageSource::request_pages rename across declarations, implementation, comments and callers. Run formatting, clang-tidy, both image boots, host tests and the pager suites, then commit.
2. Nest the existing fault bitmask in VmFault as Cause and update C++, architecture and assembly references. Verify the original values, both boots, host tests and fault-related suites, then commit.
3. Rename COW and VMO lookup cursor factories and their result record together. Update all callers and verify both boots, host tests and COW suites, then commit.
4. Rename VmArea::map_with_vmo to map_vmo across every caller. Verify both boots, host tests and mapping suites, then commit.
5. Nest the unmap policy as VmArea::UnmapScope and rename its choices MappingsOnly and IncludeSubvmas. Verify both boots and unmap suites, then commit.
6. Rename create_subaom_preamble to resolve_range_locked after all related callers are migrated. Verify both boots and placement suites, then commit.

## Files

- The affected kernel MM declarations and implementations.
- Architecture, task, object, heap and kernel command callers reached by the selected names.
- The MM design reference and change record documenting the naming decisions.

## Test plan

Use the existing compile database for LLVM 23 clang-tidy and the repository formatter for changed lines. Run the repository verify script after each increment. Both development images and all host tests must pass. Run the existing kernel command suites through the renamed normal entry points. No new behavior tests are needed for these mechanical changes.
