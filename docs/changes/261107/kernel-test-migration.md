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

## Batch 3: seven more console tests become ktest suites (2026-11-07)

**Files**
- `kernel/tests/{user_copy,irq_rcu,timer,ioport,memory_pressure,resource,bti}_test.cpp` — moved from `kernel/commands/`, one `KTEST` each.
- The two BUILD.gn files — the seven move between the targets.

**Symptom**
Latent: the same Tally/verdict boilerplate as batch 2's list, on the next seven off the size-ordered list (448-610 lines).

**Cause**
The migration is incremental.

**Change**
The established pattern. user_copy's conversion is the one with teeth: its checks run on a real user address space the test attaches its own thread to, and the whole-volume path btreefs uses makes this the first booted exercise of `Thread::Current::soft_fault()` and the capturing copies since the volume round. The audit again compared every verdict message verbatim (13/13 in user_copy, 22/22 irq_rcu, 12/12 timer, 18/18 ioport, 20/20 memory_pressure with setup failures as imperative FAILs, 16/16 resource, 33/33 bti stitched across adjacent literals). Status and Rights have no `std::formatter`, so the agents used `EXPECT_TRUE(cond, msg)` for those and named `EXPECT_EQ` only where both sides print — the committed batch-1/2 practice.

**Reference**
Batches 1-2 and the interface design; LVM round's btreefs whole-volume path is what makes user_copy's on-target run load-bearing.

**Verification**
`ktest list` 42 tests. On the dbg image: UserCopy, IrqRcu, Timer, Ioport, MemoryPressure, Resource, Bti each `ran=1 passed=1`. boot_check: dbg 46/5/0, rel 44/8/0 7/7 required, host-tests 183/0.

**Deliberately not done**
`port.test` takes run options — finale batch.

**How it was found**
None (mechanical).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `EXPECT_EQ` stay off `Status`/`Rights` comparisons in these tests?
- What runs now that the volume manager's LV serves ours.block.Block and user_copy's checks are booted?
- Why are setup failures in void helpers `FAIL` but in non-void helpers `EXPECT_TRUE(false, ...)` + return?

## Batch 4: eight more console tests become ktest suites (2026-11-07)

**Files**
- `kernel/tests/{pager,large_page,hotplug,steal,irq_revoke,page_reclaim,tick_replace,pm_node}_test.cpp` — moved from `kernel/commands/`, one `KTEST` each.
- The two BUILD.gn files — the eight move between the targets; pm_node pulls numa's and sched_domain's header surfaces, steal pulls gktl/obi/sched_domain (the steal_policy pattern).

**Symptom**
Latent: the same Tally/verdict boilerplate, on the next eight off the size-ordered list (610-947 lines).

**Cause**
The migration is incremental.

**Change**
The established pattern. Three conversions carry notes. page_reclaim's `--target-node` flag had no ktest analog: its targeted-reclaim check now runs unconditionally and its no-memory-only-node exit is a per-case skip in a log line (a whole-test KTEST_SKIP would mark the default single-node image's run skipped for ever). hotplug's helper-internal "skipped" exits stay log-and-return for the same reason -- they skip one case, not the test -- and its old leak-on-failure line is gone because EXPECT semantics let `delete state` always run. pm_node's `--exhaust-remote` is the first migration through the run-options mechanism (step 2's): `ktest::has_option("exhaust-remote")`.

The audit compared every verdict message verbatim, stitching adjacent literals: pager 19/19 (six setup failures as imperative FAILs), large_page 23/23 (three), hotplug 33/33, steal 27/27, irq_revoke 46/46, page_reclaim 32/32, tick_replace 27/27, pm_node 46/46 plus two runtime-named checks riding `"{}"`.

**Reference**
Batches 1-3 and the interface design; the run-options design (step 2 of the migration plan).

**Verification**
`ktest list` 50 tests. On the dbg image: Pager, LargePage, Hotplug, Steal, IrqRevoke, PageReclaim, TickReplace, PmNode each `ran=1 passed=1`. boot_check: dbg 46/5/0, rel 44/8/0 7/7 required, host-tests 183/0.

**Deliberately not done**
Nothing in this batch; the six large files and the finale follow.

**How it was found**
None (mechanical); the pager/large_page/hotplug agent was relaunched once after hitting the session concurrency limit.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does page_reclaim's targeted-reclaim case skip per-case instead of marking the test skipped?
- What did pm_node's migration use the run-options mechanism for?
- Why did hotplug's leak-on-failure line disappear, and why is that strictly better?
