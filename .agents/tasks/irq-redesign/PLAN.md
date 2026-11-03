# irq-redesign: review and first design decision

**Date**: 2026-11-03
**Status**: Proposed

## Request

Review the design and naming of kernel/irq, then design its replacement before implementing it. This round produces evidence and the first module-ownership decision; it changes no implementation.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Module objects and ownership | Keep genirq names and hide the machinery; explicit Line, Registration, Domain and ControllerOps with domain-owned lifetime | Explicit objects, DESIGN.md D1 (b) | Pending | |

## Steps

1. Review current interfaces, lifetime and controller assumptions against settled decisions and primary source counterparts. Produce REVIEW.md and a Proposed DESIGN.md with D1 left open. Validate the baseline using clang-format, clang-tidy, both boot images, host tests, and six IRQ kernel suites. Commit the review and proposal as one documentation change. Await the user's D1 judgement before defining the next design step.

## Files

- `.agents/tasks/irq-redesign/PLAN.md`: scope and current step.
- `.agents/tasks/irq-redesign/REVIEW.md`: evidence, findings and retained guarantees.
- `.agents/tasks/irq-redesign/DESIGN.md`: candidate objects, interfaces and open decisions.
- `.agents/tasks/irq-redesign/RESULT.md`: validation and delivery status.

## Test plan

- All 15 scoped IRQ C++ files: LLVM 23 clang-format dry run passes without editing them.
- Clang-tidy against `out/irq-design-review`: record IRQ-local diagnostics separately from existing transitive-header warnings.
- `verify --out out/irq-design-review`: build succeeds, dbg and rel report PASS, host tests report PASS.
- `kshell_run.py` on the same output: irq.test, irqchip.test, irqrevoke.test, irqregistry.test, interrupt.test and interruptline.test each report ok.
- `lint_docs.py --changed` on the four task documents: no layout violations.
