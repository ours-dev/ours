# page-source: step 1, read faults that wait for a provider

**Date**: 2026-10-23

## Delivered
- `355aa0f4` feat(ustl): the intrusive Set looks elements up by a bare key.
- `352d353f` feat(mm): PageProvider, PageSource and PageRequest in `kernel/mm`; `VmCowPages::with_source()`, `Cursor::create_read_request()`, `supply_pages_locked()`; `VmObjectPaged::with_source()` (first named `create_with_source()`) and `supply_pages()`; `pagesource.test`. Record: `docs/changes/261023/mm.md`.

## Verification
- `pagesource.test`: 32 checks pass on the dbg image.
- `usercopy.test`, `vmosnapshot.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test` pass.
- `ours-verify`: dbg PASS, rel PASS, 146 host tests pass.
- Performance: not measured.

## Deviations from the design
None.

## Found along the way
- Committing or copying a sourced VMO does not wait, a Populate mapping of one fails, and a timed-out request dumps the address space without its lock: `docs/repairs/261023/mm.md` items 1 to 3.

## Not done
- Step 2, a pager object and syscalls: not approved yet.
- COW children of a sourced VMO: refused; the decision is open in DESIGN.md.
