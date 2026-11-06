# tick-sched: steps to make TickSched the scheduler's tick

**Date**: 2026-11-06
**Status**: Proposed

## Request
The user, 2026-11-06: `TickSched` was created for the scheduler's tick and implemented roughly; they want it to work as intended and not another compromise; first a plan; then the model: Linux's, a builder's choice of two modes (periodic and nohz), the tick checks the slice, `sched_hptick` keeps the precise end, and in nohz a busy CPU always ticks. Scope ends at the tick working in both modes and both device modes, the slice check in it, the periodic balance as its first extra consumer, and the tests and documents that pin it. Not in scope: jiffies, `NO_HZ_FULL`, a nohz kick of halted CPUs, deep-idle broadcast, Fair's code.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| See DESIGN.md, Decisions | | | | |

## Steps
Each step is one commit, introduced to the user before it is started, verified as the test plan says, with a change record. The order keeps the kernel booting and every test passing at each step; steps 4 onward wait for the decisions of DESIGN.md that are still open.
0. **Baselines.** Run and keep the numbers of DESIGN.md's testing section on the four lanes, the flaky tests repeated, in `docs/perf/<YYMMDD>/tick-sched/` with `scripts/perf_stats.py`; no code. Why first: the tick touches what these tests count, and two of them already fail now and then, so a regression would hide in them.
1. **Record the decision.** Replace decision (ii) in `.agents/skills/kernel-design/references/task.md` with the model of DESIGN.md, note it in `.agents/tasks/idle-power/DESIGN.md`, and mark `.agents/tasks/periodic-balance/` forms A, B, C superseded by this task. Documents only. Why now: the code that follows contradicts the text of the skill until it is changed.
2. **The build argument.** `sched_tick_mode` (`"nohz"`, `"periodic"`) in `kernel_params.gni` and `products/BUILD.gn`, replacing `sched_nohz` (repair item 20 is closed), as a `constexpr` the code reads; `sched_hptick` gets its meaning (the precise end of a slice by the deadline). Nothing reads either for behaviour yet. The lanes of the combination table are made.
3. **Observability.** `time.tick_sets` and `time.tick_runs` per CPU, a column in `idle.stats`, accessors; the `idle.test` checks written for nohz and for periodic, reporting only. No behaviour change. Why before the tick: so that every later step can show what it did to the interrupt counts.
4. **The scheduler's tick function.** `Scheduler::Current::tick(now)`: the charge and `on_thread_tick(queued = false)` (round-robin's hook gets the accounting; Fair's is called as it stands), then the balance trigger with the per-CPU `next_balance` (inert: the knob does not exist yet). A ktest that calls it directly on a spinner and sees the reschedule asked for when the slice is used up. No caller.
5. **The tick in nohz mode (one-shot).** `TimerQueue::m_tick_deadline`, its place in `earliest_deadline_locked()`, `set_tick_deadline()`, the refusal path shared with the preemption deadline, the dispatch in `run_expired_timers()` to `TickSched::on_tick()`, the arm at a non-idle pick and the clear at the idle pick in `set_next_thread()`; with `sched_hptick` off the pick no longer sets the preemption deadline. Tests: a busy CPU takes about `HZ` ticks and an idle CPU none; `preempt.test` on the hptick-on row unchanged and written for the tick's granularity on the hptick-off row; the hotplug and `come_back()` paths; `tickreplace.test` case 6 and the pick-cost bench updated for the slot being the head.
6. **Periodic mode.** The slot is not cleared at the idle pick and is always re-armed, and `TickSched::on_tick()` calls the function for a Periodic device; the combination lanes run the same tests; `idle.test`'s quiet check stays skipped where every CPU ticks.
7. **The balance.** The knob `balance-period-ms` (the sixth knob, as `.agents/tasks/periodic-balance/PLAN.md` step 1 lists the tables), the balance trigger live, the balance round of `steal.test` and its off control, `steal.policy` showing the pass count. Repair items 22 (fixed with the setting on) and 23 (not fixed: halted CPUs are not visited) are updated.
8. **Documents.** The Time section of `references/task.md`, the `periodic-balance` documents closed, a RESULT.md for this task.

## Files
- `kernel/include/ours/time/tick_sched.hpp`, `kernel/kernel/time/tick_sched.cpp`, `kernel/include/ours/time/timer_queue.hpp`, `kernel/kernel/time/timer_queue.cpp`, `kernel/kernel/time/tick_device.cpp` (Periodic dispatch): steps 2 to 5.
- `kernel/include/ours/task/sched/scheduler.hpp`, `kernel/kernel/task/sched/scheduler.cpp`, `kernel/include/ours/task/sched/scheduler_inline.hpp`: steps 3, 4 and 6.
- `kernel/commands/idle.cpp`, `kernel/commands/tick_replace_test.cpp`, `kernel/commands/steal_test.cpp`, `kernel/commands/steal_policy.cpp`, `benches/kernel/time/preemption_deadline_bench.cpp`: steps 2, 4 and 6.
- `kernel/kernel_params.gni`, `kernel/products/BUILD.gn`, `kernel/kernel/task/sched/rr.cpp`, `kernel/include/ours/task/sched/rr.hpp`: steps 2 and 4.
- The sixth knob's tables of `.agents/tasks/periodic-balance/PLAN.md`: step 7.
- `.agents/skills/kernel-design/references/task.md`, `docs/changes/<YYMMDD>/kernel-time.md`, `kernel-task-sched.md`, `docs/perf/<YYMMDD>/tick-sched/`: each step.

## Test plan
Per DESIGN.md: baselines, then for each step the three-lane `boot_check.py` plus the periodic lane, `ktest all`, and the console tests listed there, with the invariants named for the step. A step that changes an interrupt count shows the numbers before and after.
