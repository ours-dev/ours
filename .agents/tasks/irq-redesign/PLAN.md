# irq-redesign: completed incremental delivery

**Date**: 2026-11-03
**Status**: Complete

## Request

Review kernel/irq, design before rewriting, then fulfill the user's six-part design/implementation goal. The user selected B, suspended learning exercises and later selected the canonical HIrqNum/VIrqNum spelling. The accepted design is docs/design/261103/irq-redesign.md. Independent address work is preserved and is not part of this task.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Objects | Existing public descriptor/table scaffold; explicit owned roles | B | None | User selected B |
| Completion | Tokens; controller exclusion; current owner-state revalidation | Revalidation under L1 -> L2 | Waived | 3972cbb9 |
| Controller contract | Trigger-derived flow; arbitrary callback; explicit protocol | Validated protocol | Waived | a4b6b080 |
| Type spelling | Prototype names; HIrqNum/VIrqNum | User choice with scoped types | None | User selected HIrqNum/VIrqNum; b109dca0 |

## Steps

1. 02d20ed6 and 76a75be5: review and design B accepted before implementation.
2. 29198710: migrate role names and callers, preserving behavior; build/boot/tests/tidy pass.
3. fc11366b: immutable acquisition policy validated before side effects; regression cases pass.
4. 8c19c7bf: actual Guard for every dispatch, including Permanent; guarded state and preemption checks pass.
5. 0786ae6b: Domain owns transactional maps/boxed lines; layout/refusal and ownership checks pass.
6. 3972cbb9: readiness revalidation under L1 -> L2; both deterministic ack regressions fail without the gate and pass with it.
7. 3c669864: private owning directory and public dispatch endpoint; context/isolation/removal tests pass.
8. b6cda258: constrained shared control resolution; duplicate Domain control facade removed and existing contract tests pass.
9. a4b6b080: explicit controller handshakes/masking capability; 49 controller checks pass.
10. 891797e9: distinct source/id/vector identities and explicit ABI conversions; compile-time interface checks pass.
11. 8a96f1e2: private storage/callback invocation and reduced interfaces; all nine suites pass.
12. b109dca0: user-selected HIrqNum/VIrqNum spelling, independently verified in the project-local isolated worktree.
13. 561afe94: separate trap entry, restore nested frames and remove empty init calls; 82 IRQ checks and the counterfactual pass/fail proof complete.
14. Final documentation increment: reconcile accepted design, current review and six-requirement acceptance evidence; lint all changed documents and retain final verification evidence.

## Files

- docs/design/261103/irq-redesign.md: accepted current design and alternatives.
- .agents/tasks/irq-redesign/REVIEW.md: final current-state review.
- .agents/tasks/irq-redesign/STATUS.md: six-requirement evidence ledger.
- .agents/tasks/irq-redesign/RESULT.md: delivered commits, verification and scope limits.
- docs/changes/261103/kernel-irq.md: append-only per-increment records.
- Public IRQ interfaces, private kernel/kernel/irq implementation and real callers/tests: final implementation; no unrelated address or Fair changes.

## Test plan

Completed: changed-line formatting and actual clang-tidy for every changed C++ file; repository GN formatting; both x86 boot images; 161 host tests; all nine IRQ suites. Both completion-gate counterfactuals and the nested-frame counterfactual fail the intended checks, and restored fixed sources pass. Final source searches confirm no IRQ analysis escapes, permanent bypass, public storage pointer getter, legacy compatibility aliases or empty IRQ init calls. Existing external repairs are explicitly retained in RESULT.md.
