# protocols-library: the round's three steps

**Date**: 2026-11-04
**Status**: Proposed

## Request

The user's consolidation: all wire protocols in one `protocols` library,
namespace `prot` — OS-layer contracts leave the kernel ABI's namespace.


## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| See DESIGN.md's table | — | — | — | all four settled 2026-11-04 |
## Steps

1. **The library.** `system/lib/protocols`: six codec headers under
   `include/prot/`, the three client headers, two codec sources, tests moved
   and renamed, one BUILD.gn with the headers/client/source_set/tests shape.
   Verified by the host tests building green.
2. **The tree.** Every include (`<ours_X/...>` → `<prot/...>`) and namespace
   (`ours_X::` → `prot::<family>::`) rebound across the tree; every BUILD.gn
   dep on the six libs → `//system/lib/protocols` (subtargets renamed to
   match); the six old directories deleted. Verified by a full
   `boot_check.py` (dbg + rel + host) green, diskless, then dual-disk.
3. **The record.** Abstraction-audit findings, the change record
   (`docs/changes/261104/protocols.md`), RESULT.md.

## Files

- Created: `system/lib/protocols/**`, `.agents/tasks/protocols-library/**`.
- Deleted: `system/lib/ours.{block,io,resource,acpi,fshost,process}/**`.
- Changed: every file that included a protocol header (~67), every BUILD.gn
  that dep'd one (~57 targets), `//:default`'s tests group.

## Test plan

Every step green on `boot_check.py --out out/protocols.x86`; step 2 also
disked (dual-disk, `DISK=` + `NVME_DISK=`). clang-format on every changed
C++ file; pre-commit green.
