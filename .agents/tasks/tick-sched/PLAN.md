# tick-sched: steps to make TickSched the scheduler's tick

**Date**: 2026-11-06
**Status**: Proposed

## Request
The user, 2026-11-06: `TickSched` was created for the scheduler's tick and implemented roughly; they want it to work as intended and not another compromise; first a plan. Scope ends at the tick working in both device modes, the periodic balance as its first consumer, and the tests and documents that pin it. Not in scope: the end of a slice, jiffies, the discipline's `on_thread_tick`, a nohz kick of halted CPUs, deep-idle broadcast.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| See DESIGN.md, Decisions | | | | |

## Steps
Each step is one commit, introduced to the user before it is started, verified as the test plan says, with a change record. Steps 3 onward wait for the decisions of DESIGN.md.
0. **Baselines.** Run and keep the numbers of DESIGN.md's testing section on the four lanes, the flaky tests repeated, in `docs/perf/<YYMMDD>/tick-sched/` with `scripts/perf_stats.py`; no code. Why first: the tick touches what these tests count, and two of them already fail now and then, so a regression would hide in them.
1. **Record the decision.** Amend decision (ii) in `.agents/skills/kernel-design/references/task.md` and note it in `.agents/tasks/idle-power/DESIGN.md`; mark `.agents/tasks/periodic-balance/` forms A, B, C superseded by this task. Documents only. Why now: the code that follows contradicts the text of the skill until it is changed.
2. **Observability.** `time.tick_sets` and `time.tick_runs` per CPU, a column in `idle.stats`, accessors; the `idle.test` check for a busy CPU written but reporting only. No behaviour change. Why before the tick: so that every later step can show what it did to the interrupt counts.
3. **The scheduler's tick function.** `Scheduler::Current::tick(now)` with the per-CPU `next_balance`, the balance trigger inside (the knob does not exist yet, so it is inert), and `TickSched::wanted()` as the set of consumers; a ktest that calls the function directly. No caller.
4. **The tick slot (one-shot).** `TimerQueue::m_tick_deadline`, its place in `earliest_deadline_locked()`, `set_tick_deadline()`, the refusal path shared with the preemption deadline, the dispatch in `run_expired_timers()` to `TickSched::on_tick()`, the arm and clear in `set_next_thread()`. `wanted()` is false until a test enables it, so the kernel behaves as before. Tests: with a test consumer on, a busy CPU takes about `HZ` ticks a second and an idle CPU none; `preempt.test` unchanged; the hotplug and `come_back()` paths; `tickreplace.test` case 6 and the pick-cost bench updated for the slot being the head.
5. **Periodic mode.** `TickSched::on_tick()` calls the function when the CPU is busy and a consumer wants it; the `time_oneshot=false` lane runs the same tests; `idle.test`'s quiet check stays skipped there, as it is.
6. **The balance.** The knob `balance-period-ms` (the sixth knob, as `.agents/tasks/periodic-balance/PLAN.md` step 1 lists the tables), the consumer switching `wanted()`, the balance round of `steal.test` and its off control, `steal.policy` showing the pass count. Repair items 22 (fixed with the setting on) and 23 (not fixed: halted CPUs are not visited) are updated.
7. **Documents.** The Time section of `references/task.md`, the `periodic-balance` documents closed, `docs/changes` for each step already written with the step, a RESULT.md for this task.

## Files
- `kernel/include/ours/time/tick_sched.hpp`, `kernel/kernel/time/tick_sched.cpp`, `kernel/include/ours/time/timer_queue.hpp`, `kernel/kernel/time/timer_queue.cpp`, `kernel/kernel/time/tick_device.cpp` (Periodic dispatch): steps 2 to 5.
- `kernel/include/ours/task/sched/scheduler.hpp`, `kernel/kernel/task/sched/scheduler.cpp`, `kernel/include/ours/task/sched/scheduler_inline.hpp`: steps 3, 4 and 6.
- `kernel/commands/idle.cpp`, `kernel/commands/tick_replace_test.cpp`, `kernel/commands/steal_test.cpp`, `kernel/commands/steal_policy.cpp`, `benches/kernel/time/preemption_deadline_bench.cpp`: steps 2, 4 and 6.
- The sixth knob's tables of `.agents/tasks/periodic-balance/PLAN.md`: step 6.
- `.agents/skills/kernel-design/references/task.md`, `docs/changes/<YYMMDD>/kernel-time.md`, `kernel-task-sched.md`, `docs/perf/<YYMMDD>/tick-sched/`: each step.

## Test plan
Per DESIGN.md: baselines, then for each step the three-lane `boot_check.py` plus the periodic lane, `ktest all`, and the console tests listed there, with the invariants named for the step. A step that changes an interrupt count shows the numbers before and after.
