# irq-redesign: implementation progress and acceptance evidence

**Date**: 2026-11-03

## Goal and execution

Refactor the interrupt subsystem until its concepts and implementation satisfy the user's six requirements. D1 (b) is accepted. The user explicitly suspended learning-oriented arguments and requested focused development on 2026-11-03. Do not wait for predictions, concurrency exercises or handwritten milestone functions during this task; make and record engineering arguments, test them, and continue. The active goal is not paused.

The preceding turn made authoritative progress by recording D1 and verifying the baseline. This turn begins implementation from the inspected worktree. Unrelated deleted Boost vendor paths and .mimosa are outside this work.

## Acceptance ledger

| Requirement | Required evidence | Current state |
|---|---|---|
| 1. No meaningless or redundant names, objects or methods | One role per public object; no old-name compatibility aliases; domain owns its source storage; no duplicated public control facade | Partial: Line, Registration, ControllerOps and ControllerSource migrated; domain consolidation remains |
| 2. No evasion of required guards | Every mutable line path uses lockdep::Guard and checked TA_GUARDED/TA_REQUIRES; no suppressed permanent-dispatch read; lock-order argument and stress coverage | Incomplete: existing Permanent bypass remains to be removed |
| 3. Independent reasoning | Each invariant and protocol justified from owned state, callers and controller behavior; references used for cross-checking rather than as authority for shape | Partial: review established the invariant inventory; final protocol argument remains |
| 4. Responsible extension | Typed source and line identities; per-controller context; validated dispatch capability; safe dynamic domain removal; alternate-adapter contract tests | Incomplete: numeric types, capabilities and owning domain still required |
| 5. Use C++ to remove mechanical repetition | Shared safe resolution of line control, explicit noncopyable ownership, constrained or constexpr validation where it enforces real rules | Incomplete: forwarding table and owning Span convention remain |
| 6. Compare other mechanisms and designs | Compare concrete completion and controller-protocol alternatives with costs, then implement the chosen mechanism and verify its invariant | Incomplete: comparison and final mechanism remain |

## Verified increments

- 29198710, object vocabulary migration: six IRQ implementation roles and all affected callers compile with Line, Registration, ControllerOps and ControllerSource. No compatibility aliases.
- LLVM 23 formatting and clang-tidy ran on all 22 touched C++ files; no new changed-line diagnostics.
- Build and both boot images pass in out/irq-design-review; 161 host tests pass.
- Nine kernel suites pass: irq.test 72, irqcontroller.test 18, irqrevoke.test 37, irqregistry.test 66, irqdomain.test 87, irqrcu.test 34, interrupt.test 67, interruptline.test 78, pinlines.test 28.

- Registration policy validation: both attach paths reject Permanent | Shared and unknown bits before state changes; irq.test now passes 76 checks, irqcontroller.test 18 and interruptline.test 78. Both images and 161 host tests pass.

## Current work

Commit the verified registration-policy fix, then remove the unguarded Permanent path before consolidating domain ownership and closing the completion protocol. Naming is an initial migration, not a substitute for the full goal. Final completion requires auditing every ledger row against current source and current verification.
