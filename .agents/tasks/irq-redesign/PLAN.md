# irq-redesign: review and first design decision

**Date**: 2026-11-03
**Status**: Proposed

## Request

Review the design and naming of kernel/irq, then design its replacement before implementing it. The user subsequently set the full six-part refactoring goal and suspended learning-oriented arguments. Implement the accepted object model and resolve the remaining engineering decisions autonomously, with per-increment formatting, tests, both boot checks, review and commits.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Module objects and ownership | Keep genirq names and hide the machinery; explicit Line, Registration, Domain and ControllerOps with domain-owned lifetime | Explicit objects, DESIGN.md D1 (b) | Not supplied | (b), 2026-11-03: "B" |

## Steps

1. Completed in 02d20ed6: review current interfaces, lifetime and controller assumptions against settled decisions and primary source counterparts; record REVIEW.md and Proposed DESIGN.md. The baseline passed clang-format, clang-tidy, both boot images, host tests and six IRQ kernel suites.
2. Completed in 76a75be5: record D1 (b) and the completion ordering. The user subsequently suspended learning exercises.
3. Migrate the accepted object vocabulary through definitions, callers, build files and tests. Validate all touched C++ files, both images and nine IRQ suites; commit this naming increment before changing semantics. Remaining acceptance criteria are tracked in STATUS.md.

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

### Final increment: separate entry bookkeeping and restore nested frames

Move receiving-CPU entry state and root-handler publication into entry.hpp/entry.cpp, leaving mod.hpp/mod.cpp responsible for line controls. enter_trap returns the previous frame; leave_trap restores it before dropping disable counts. This preserves nested exception context without adding a parallel depth counter. Remove the two empty initialization stubs and the no-op call while retaining the real InitLevel::Irq hook dispatch. Validate a nested pair under disabled interrupts/preemption: inner exit restores outer frame and both disable counts, outer exit restores the original frame and counts. Run formatting/tidy, both images, host tests and all IRQ suites before the isolated commit.
