# irq-redesign: implementation progress and acceptance evidence

**Date**: 2026-11-03

## Goal and execution

Refactor the interrupt subsystem until its concepts and implementation satisfy the user's six requirements. D1 (b) is accepted. The user explicitly suspended learning-oriented arguments and requested focused development on 2026-11-03. Do not wait for predictions, concurrency exercises or handwritten milestone functions during this task; make and record engineering arguments, test them, and continue. The active goal is not paused.

The preceding turn made authoritative progress by recording D1 and verifying the baseline. This turn begins implementation from the inspected worktree. Unrelated deleted Boost vendor paths and .mimosa are outside this work.

## Acceptance ledger

| Requirement | Required evidence | Current state |
|---|---|---|
| 1. No meaningless or redundant names, objects or methods | One role per public object; no old-name compatibility aliases; domain owns its source storage; no duplicated public control facade | Partial: role names migrated and Domain now owns mappings and line storage; public raw pointer exposure and duplicate Domain controls removed; final naming audit remains |
| 2. No evasion of required guards | Every mutable line path uses lockdep::Guard and checked TA_GUARDED/TA_REQUIRES; no suppressed permanent-dispatch read; lock-order argument and stress coverage | Implemented for live line state: all dispatches use Guard, all flow helpers require L1, flags are TA_GUARDED, and no IRQ analysis suppressions remain; final lock-order and stress audit still required |
| 3. Independent reasoning | Each invariant and protocol justified from owned state, callers and controller behavior; references used for cross-checking rather than as authority for shape | Partial: review established the invariant inventory; final protocol argument remains |
| 4. Responsible extension | Typed source and line identities; per-controller context; validated dispatch capability; safe dynamic domain removal; alternate-adapter contract tests | Incomplete: owning domains, per-controller context and endpoint isolation are implemented; validated handshake protocols and explicit deferred-mask capability are implemented; HardwareIrq/IrqId are distinct scoped enums and real interface rejection is compile-time checked; final audit remains |
| 5. Use C++ to remove mechanical repetition | Shared safe resolution of line control, explicit noncopyable ownership, constrained or constexpr validation where it enforces real rules | Partial: DescTable and owning Span convention removed; private Storage uses existing Vec and Box; with_line shares resolution, error propagation and reader lifetime through a constrained operation |
| 6. Compare other mechanisms and designs | Compare concrete completion and controller-protocol alternatives with costs, then implement the chosen mechanism and verify its invariant | Partial: three completion alternatives compared; owner-state revalidation implemented and counterfactually tested; controller protocol compared and implemented; final audit remains |

## Verified increments

- 29198710, object vocabulary migration: six IRQ implementation roles and all affected callers compile with Line, Registration, ControllerOps and ControllerSource. No compatibility aliases.
- LLVM 23 formatting and clang-tidy ran on all 22 touched C++ files; no new changed-line diagnostics.
- Build and both boot images pass in out/irq-design-review; 161 host tests pass.
- Nine kernel suites pass: irq.test 72, irqcontroller.test 18, irqrevoke.test 37, irqregistry.test 66, irqdomain.test 87, irqrcu.test 34, interrupt.test 67, interruptline.test 78, pinlines.test 28.

- Registration policy validation: both attach paths reject Permanent | Shared and unknown bits before state changes; irq.test now passes 76 checks, irqcontroller.test 18 and interruptline.test 78. Both images and 161 host tests pass.

- Guarded dispatch: the Permanent callback observes a held spin lock and disabled preemption; irq.test passes 77 checks and all nine suites pass. The line flags are no longer atomic because all accesses use L1. Both boot images and 161 host tests pass.

- Owning Domain: DescTable is removed. Transactional Storage uses Vec for both maps and Vec<Box<Line>> for lines. Domain copy and move are rejected at compile time. Both images, 161 host tests, all 14 changed C++ checks and all nine IRQ suites pass, including 87 Domain layout/refusal checks.

- Completion revalidation: the line queries current owner state under L1 -> L2; deferred registration requires readiness in its named callback record. irq.test passes 78 checks, interruptline.test 83, and all nine suites pass. Removing only the gate fails both new ack interleaving checks; the restored fixed source is rebuilt and verified.

## Current work

The completion protocol is committed as 3972cbb9. DomainEndpoint now hides raw domain access and the private registry expresses unique ownership; 85 registry checks pass, including 19 public API isolation checks. Common control resolution is now implemented and verified; typed identities are now verified; next complete the final interface and naming audit. The goal remains active until every acceptance row is proved. Naming is an initial migration, not a substitute for the full goal. Final completion requires auditing every ledger row against current source and current verification.

- Owning endpoint: 19 changed C++ files pass formatting/tidy; both images, 161 host tests and all nine suites pass. Registry checks increase from 66 to 85. Static/private ownership and multi-CPU retirement review found no unresolved change-local issue.

- Common controls: nine changed C++ files pass format/tidy; both images, 161 host tests and all nine suites pass. Domain no longer mirrors the ordinary control facade.

- Controller protocol: electrical trigger no longer selects a handshake. Eoi and AckEoi are validated independently of Edge/Level; synchronous deferred masking is explicit. irqcontroller.test passes 49 checks, with 32 new contract checks and the unsupported affinity-stub check removed. Fifteen C++ files pass format/tidy; both images and 161 host tests pass.

- Typed identities: 39 changed C++ files pass formatting and actual clang-tidy. Both x86 images, 161 host tests and all nine IRQ suites pass. No old aliases remain in C++ source; syscall wire format remains u32. Directed EOI now takes IrqVec.

- Public boundary: Line/Domain/registry are private; callbacks are invoked only by L1-required Line members. Unused names, initial Line policy, allocation-only factory, root-only mapping wrapper and invalid unbound LAPIC table are removed. Twenty-eight C++ files, both images, 161 host tests and all nine suites pass. Trap-entry bookkeeping and final document reconciliation remain.
