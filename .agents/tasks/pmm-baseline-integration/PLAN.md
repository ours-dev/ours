# pmm-baseline-integration: integrate the MM naming and repair branch

**Date**: 2026-11-05
**Status**: Accepted

## Request

Merge codex/pmm-naming into baseline/20260824. Preserve the baseline's kernel-test framework migration and the branch's approved PMM/VMM names, responsibility fixes and regressions.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Integration direction | Feature first, baseline first | Baseline into feature first | None | Required by merge skill |
| Migrated tests | Restore old commands, move branch checks into ktest | Preserve the current ktest interface and port all branch checks | None | Existing baseline test migration defines the target interface |
| Shared checkout | Stash unrelated work, preserve noncolliding files in place | Preserve files in place | None | Four untracked baseline files, no staged work and no path collisions |

## Steps

1. Merge baseline/20260824 into codex/pmm-naming without committing. Resolve naming and test migration conflicts using both branches' change records.
2. Format and lint all resolutions, build and boot both development images, run host tests and exercise the affected MM suites. Verify four-node NUMA allocation behavior.
3. Commit the verified integration on the feature branch. Recheck the shared baseline checkout, merge the feature branch there with a merge commit, and confirm that its tree matches the verified feature tree.
4. Record the integration commit and preservation of the baseline's pre-existing untracked files.

## Files

- Conflicted kernel test sources and their migrated ktest equivalents.
- Naming and verification skill files with independent changes on both sides.
- The existing MM changes and tests that need updated names in the baseline's new test directory.
- This task's integration records.

## Test plan

Use the repository formatter and LLVM 23 clang-tidy on C++ resolutions. Build and boot the default and four-node NUMA images with the existing test disk and run all host tests. Run ktest all and remaining legacy MM commands, including opt-in allocation failure cases when their build is available. Reuse a verified tree for the final baseline merge only after an empty tree diff proves the merge landed it unchanged.

The starting feature tip is 59046bdc and the starting baseline tip is 80958189. The baseline has no staged changes and four untracked files under reports and research_notes. The feature worktree also contains an unrelated untracked vmm-flags-review directory.

The naming follow-up integrates feature tip 5070f014 (019af217 Watermark and 5070f014 policy) with baseline tip 8fb7e131. The baseline adds preemption and hotplug test fixes. Its five modified tracked files and five untracked files have no merge-path overlap and no staged work; retain them in place. Verify the combined default and four-node trees, run the two baseline test fixes and PMM suites, then compare the final baseline tree with the verified feature tree. This follow-up repeats steps 1 through 4 without redoing the earlier KTEST migration.
