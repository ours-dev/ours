# task-scheduler-layout: Move scheduler code directly into task

**Date**: 2026-11-07
**Status**: Accepted

## Request

Move the scheduler files directly into the task directories and place their declarations in the task namespace.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Layout | Scheduler subdirectory and namespace or direct task ownership | Direct task ownership | None | Direct task ownership, 2026-11-07 |
| Colliding headers | Merge unrelated headers or name scheduler headers for their role | Use the scheduler prefix for generic header names | None | Scheduler prefix, implementation choice |

## Steps

1. Move the scheduler files and update include paths, qualified names, debugger lookups and the GN label. Verify both scheduling disciplines before committing the layout migration.

## Files

- `kernel/include/ours/task/` holds the moved scheduler headers and the scheduler class.
- `kernel/kernel/task/` holds the moved scheduler sources and the GN target.
- Kernel callers, tests and benchmarks use the new headers and namespace.
- `scripts/lldb_ours.py` resolves the moved scheduler and the canonical thread state field.
- The kernel design reference and change record capture the new locations.

## Test plan

Check for live references to the old paths and namespace. Run changed-line clang-format, clang-tidy and GN formatting. Build and boot dbg and rel with both RR and Fair, and run the host suites. Check the scheduler and thread state names in the debug image.
