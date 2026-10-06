# irq-genirq: the steps that rebuild kernel/irq around line descriptors, flows and ONESHOT

**Date**: 2026-10-05
**Status**: Accepted

## Request
On 2026-10-05 the user chose to rebuild kernel/irq in the style of Linux's genirq, and asked which of its responsibilities could move into the interrupt objects (DESIGN.md, Problem). Shown the design, they answered "直接做": every decision D1-D10 is taken as recommended. The prediction for D1 and the concurrency argument for D2 are skipped; the finalize exercise and interleaving B stay open in DESIGN.md's Open questions.

The scope ends at DESIGN.md's Goals: genirq's roles at "basically usable", one EOI per vector through the chip, ONESHOT owned by the line, and interrupt objects that no longer write the controller mask. Its Non-goals and its Deferred list are out of scope.

## Decisions
None. The plan follows DESIGN.md's decisions D1-D10, all taken as recommended on 2026-10-05.

## Steps
The steps keep DESIGN.md's Staging numbers. Each joins this list when it starts, as the Staging says, so a step not listed here has not started.

Every step:
- lands as one commit, made with `git commit -- <paths>`, with its entry in `docs/changes/<YYMMDD>/kernel-irq.md` in the same commit;
- builds both images and boots them under `python3 .agents/skills/ours-verify/scripts/boot_check.py`, and the rel image still logs `pc-ps2: key a` and `pc-ps2: key A`;
- then runs `python3 scripts/kshell_run.py --out out/bringup.x86 irq.test interruptline.test interrupt.test` on dbg, every test ok.

### 1. Record the exception
- What: `ours-kernel-design`'s `references/task.md` gains "Interrupt lines: kernel/irq follows genirq"; the skill's index points interrupt-line work at it; DESIGN.md is Accepted, with every Decided cell filled.
- Why now: every later step departs from Zircon, the skill's default spec. Without the rule beside that default, a reviewer of step 3 or step 7 would revert the change as drift.
- Verified: `python3 scripts/lint_docs.py --changed` on the documents. Docs only; in review both images were built and booted once (dbg and rel PASS, rel with both `pc-ps2: key` lines) and the three kernel tests passed, as the baseline for step 2.
- Module: `kernel-irq`.
- Commit: `docs(irq): accept the genirq design and record kernel/irq as a Linux exception` (933d0004), with the review's fixup.

## Files
- `.agents/skills/ours-kernel-design/references/task.md`, `.agents/skills/ours-kernel-design/SKILL.md` — step 1, the recorded exception
- `.agents/tasks/irq-genirq/DESIGN.md` — step 1, Accepted
- `docs/changes/261005/kernel-irq.md` — every step's change record
- Later steps add their files here as they start.

## Test plan
- Every step: boot_check prints `dbg PASS` and `rel PASS` with `6/6 required`, and kshell_run prints `irq.test: ok`, `interruptline.test: ok` and `interrupt.test: ok`.
- The checks the steps add are DESIGN.md's Testing and verification, each with the step that brings its mechanism: in `irq.test`, flow selection and a no-action line masked (step 4), then held set by Oneshot and cleared by finalize, detach clearing held, Permanent with Oneshot refused and WakeThread not counted as spurious (step 6); in `interruptline.test`, the level and edge mask read-backs (step 5, passing again after step 7).
