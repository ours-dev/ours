# kernel-test-migration changes

## Batch 2: six more console tests become ktest suites (2026-11-07)

**Files**
- `kernel/tests/{preempt,sleep,pin_lines,page_queues,irq_controller,bti_stress}_test.cpp` — moved from `kernel/commands/`, one `KTEST` each, every check preserved (the audit diffed every verdict message against the new file's strings).
- `kernel/commands/BUILD.gn`, `kernel/tests/BUILD.gn` — the six move between the targets; the `kernel.tests` aggregate links them.

**Symptom**
Latent: the six tests each carried their own Tally/verdict reporting, appeared in no `ktest list`, and rode the console runner's legacy verdict rule.

**Cause**
The migration is incremental; these are the next six off the size-ordered list (346-431 lines). `thread_join` and `timer_share` were in this batch's size range but take run options, so they wait for the finale batch that uses the options mechanism.

**Change**
The batch-1 pattern, unchanged: Tally/verdict become `EXPECT_*`/`ASSERT_*`/`FAIL`/`KTEST_SKIP`; helpers that took a tally drop it; allocation failures in the test body are `ASSERT_NONNULL`, in helpers `FAIL`; diagnostic logs stay with a `name:` prefix; the command body is `KTEST(Suite, Name)`.

Two cases name the pattern's edges. `Preempt.SlicesEndAtTheDeadline` is `KTEST_SKIP`d whole: its bounds were measured on 2026-11-05, and the periodic balance (form T) and steal work landed after -- the baseline control ran the OLD console command on baseline and failed the same check (longest turn 68371 us against a 68000 bound), so the skip is the scheduler round's debt made visible, not a migration weakening (docs/repairs/261107/preempt-bounds.md carries the numbers). And irq_controller's `answered()` helper passes its check name through the assertion: an assertion's message is a consteval format string, so the runtime name rides as `"{}"`'s argument.

**Reference**
Batch 1 (`f44d9634`) is the pattern; `EXPECT_EQ(want, got, "{}", name)` is the interface's own format-string rule (docs/design/261105/kernel-test-framework.md).

**Verification**
`ktest list` 35 tests (29 + these six). On the dbg image: Sleep, PinLines, PageQueues, IrqController, BtiStress each `ran=1 passed=1`; Preempt `ran=1 skipped=1` (the skip above). boot_check: dbg 46/5/0, rel 44/8/0 with 7/7 required, host-tests 183/0. The audit script compared every old verdict message against the new files: irq_controller 21/21 verbatim, page_queues 18/18 (three setup failures reworded to the FAIL imperative), sleep 15/15, pin_lines 29/29 records with one verdict-false become KTEST_SKIP (machine shape: no pin past the ISA range), bti_stress 10/10.

**Deliberately not done**
`thread_join` and `timer_share`: they take run options and move in the finale batch.

**How it was found**
The Preempt failure was found by the migration's own per-suite run and isolated by the baseline control; the format-string compile error was found by the build, and the subagent converting sleep/pin_lines had flagged the same construct before it compiled.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is Preempt skipped rather than converted with wider bounds, and what would widening it silently have hidden?
- Why can an assertion's message not take a `char const*` variable, and what is the pattern for a runtime name?
- What does the audit script compare, and why is a message-count diff alone not enough?
