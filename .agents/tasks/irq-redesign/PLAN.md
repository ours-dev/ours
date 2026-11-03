# irq-redesign: review and first design decision

**Date**: 2026-11-03
**Status**: Proposed

## Request

Review the design and naming of kernel/irq, then design its replacement before implementing it. The review and first module-ownership decision are recorded; the current design step is the deferred-completion invariant. No implementation change is authorized.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Module objects and ownership | Keep genirq names and hide the machinery; explicit Line, Registration, Domain and ControllerOps with domain-owned lifetime | Explicit objects, DESIGN.md D1 (b) | Not supplied | (b), 2026-11-03: "B" |

## Steps

1. Completed in 02d20ed6: review current interfaces, lifetime and controller assumptions against settled decisions and primary source counterparts; record REVIEW.md and Proposed DESIGN.md. The baseline passed clang-format, clang-tidy, both boot images, host tests and six IRQ kernel suites.
2. Record the user's D1 (b) choice and teach the D2 objects and event ordering. Await the user's concurrency argument before analyzing mechanisms. Validate documentation layout and run verify before committing this documentation update. No completion mechanism or implementation is selected.

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
