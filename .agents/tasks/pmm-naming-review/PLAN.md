# pmm-naming-review: propose focused PMM interface names

**Date**: 2026-11-05
**Status**: Implemented

## Request

Review PMM naming and list the current and proposed names for the user's selection. Apply Rust-based interface conventions and consult the existing PMM design decisions and local kernel references.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Scope | Propose names, implement names | Propose names first | None | User selected the candidate-list review |
| Principal model vocabulary | Existing PmFrame, PmZone, PmNode and Gaf, new type family | Existing vocabulary | None | Retain the previously selected resource and allocation flag names |

## Steps

1. Read the resource and allocator entry points and trace names whose apparent meaning differs from their implementation.
2. Compare the unit and ownership terminology with local reference implementations, then check the reviewed C++ files with LLVM 23 formatting and clang-tidy.
3. Record a numbered candidate list with exact source locations, leaving implementation decisions to the user.

## Files

- PMM resource, allocation request, buddy and allocation-state declarations and their implementations are reviewed.
- REVIEW.md records the proposed names and evidence.

## Test plan

This is a source review. Run LLVM 23 clang-format in dry-run mode and clang-tidy against the existing compile database for the nine reviewed files. Confirm that the existing built development images and host tests still pass before committing the report. No implementation or new behavior tests are part of this review.

The user selected PmZone::num_free_frames for the counter getter and requested a comment explaining its snapshot contract. This follow-up migrates the callers, keeps the existing counter and lock behavior, and runs the normal C++ and build-and-boot checks plus the existing PMM commands.
