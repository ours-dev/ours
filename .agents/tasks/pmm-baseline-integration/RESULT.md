# pmm-baseline-integration: integrated the MM naming branch with the baseline

**Date**: 2026-11-05

## Delivered

Integrated the feature tip `59046bdc` with baseline tip `80958189`. The enclosing merge commit preserves the approved PMM/VMM names and responsibility repairs, while adopting the baseline's KTEST framework and the independent baseline changes. All 176 assertion sites in seven MM suites were migrated, including allocation-failure regressions. Removed the four remaining legacy test files whose registrations the baseline had already replaced.

The naming follow-up integrates `019af217` (PmZone::Watermark) and `5070f014` (AllocState::policy) with baseline `8fb7e131`, including its preemption and hotplug test fixes. The merge has no conflicts. Five tracked work-in-progress files and five untracked files in the shared baseline checkout are retained in place, without stashing.

## Verification

- Default and four-node NUMA builds: debug image 46 passed, 5 skipped; release image 44 passed, 8 skipped, with all 7 required device/routing checks. Each configuration passed all 173 host tests.
- Fault-enabled coverage build: both images and all 173 host tests passed.
- Default and four-node `ktest all`: 24 ran, 23 passed, 1 intentional skip, zero failed. The four-node PMM command passed 77 checks; the default PMM command passed 38.
- Eleven additional default-image commands passed: preempt, pagesource, pager, largepage, pagequeues, pagereclaim, mempressure, bti, btistress, pinlines and usercopy. The default snapshot command passed 264 checks.
- The first full instrumented run completed every migrated test and every MM command, but failed preempt.test's timing assertions. A separate fresh-boot instrumented attempt still missed one timer bound. A final full run placed preempt.test first and completed all 31 commands successfully: all 24 registered tests and all 30 remaining legacy commands. These passes do not establish that the timing instability is fixed.
- The fault-enabled KTEST summary was 24 ran, 23 passed, 1 skipped, zero failed. kshell_run.py separately rejects three intentional page-table error logs; cov_kernel.py reports the test verdicts successfully.
- VMM coverage, using the existing source grouping: 4375/4772 lines (91.68%), 1800/2244 branches (80.21%), 447/483 functions (92.55%). The 95% line target still needs 159 additional covered lines.
- Repository changed-line formatting and LLVM 23 clang-tidy passed on all twelve adapted test sources in both configurations. Documentation lint passed. Performance was not measured.

The naming follow-up passed both image boots and 173 host tests in both default and four-node configurations. Debug reported 46 passed and 5 skipped; release reported 44 passed and 8 skipped with 7/7 required checks. Default preempt.test passed 8 checks, hotplug.test 47, pmnode.test 38, and KTEST reported 24 ran, 23 passed, 1 intentional skip. Four-node pmnode.test passed 77 checks and the zone KTEST passed one case. The two incoming test files passed formatting and clang-tidy with no diagnostics on their changed lines.

## Deviations from the design

None.

## Found along the way

The new assertion API requires a literal format string when a description is dynamic; corrected the fault-injection test accordingly. The generic console runner's treatment of expected errors is tracked in `docs/repairs/261105/kernel-test-framework.md`. The timing instability is tracked in `docs/repairs/261026/kernel-time.md`.

## Not done

The 95% VMM coverage target and timer diagnosis remain separate work. The final baseline merge commit is reported in the conversation; its source tree must match this verified feature tip exactly. Four unrelated baseline files under reports and research_notes are retained, without stashing, and their SHA-256 values are checked around the final merge. The feature worktree's unrelated vmm-flags-review directory is excluded.
