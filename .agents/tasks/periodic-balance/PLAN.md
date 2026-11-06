# periodic-balance: steps to add the periodic balance

**Date**: 2026-11-06
**Status**: Proposed

## Request
The user, 2026-11-06: allow work stealing when a CPU is not idle, as a periodic load-balancing worker; then chose "a global worker thread" (form A). A review of the design the same day found that a worker thread counts in the load it balances; the form is open again (DESIGN.md, Decisions). The steps below are for form C (the slice end), the recommendation after reading `TickSched`; form B (a timer on each CPU) and form A are noted where they differ. Scope ends at a balance that is off by default, switched on by a knob, with its test; the measurement of it is the continuation of `.agents/tasks/steal-policy-measure/` (W3 and W4 with the period in the sweep).

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| See DESIGN.md | | | | |

## Steps
1. **The knob.** `Knob::BalancePeriodMs` (default 0, 0 to 10000), its boot option, the sixth entry in every table that pins the five (`NUM_KNOBS`, `knob_info`, `STEAL_OPTIONS`, `NUM_STEAL_OPTIONS`, the initializer of `BootOptions::m_steal` with an assertion that an unset sixth option reads as none, `steal_policy.cpp`, the ktests and host tests, the knob pin list of `steal.test`, `steal.bench`'s knob loops), and `steal.policy`. Why first: plumbing that nothing reads, so nothing changes. Verified with the host and ktests, `steal.policy` on the lanes, the 12 console tests. One commit.
2. **The pass at the slice end (form C).** A per-CPU `next_balance` and the call in `Scheduler::Current::on_preemption_deadline()`; a pass counter shown by `steal.policy`. (Form B instead: a per-CPU periodic timer, armed through `IpiService::sync_exec()` and by `StealPolicy::set()` under `cpu_hotplug_lock()` when the knob leaves 0, and cancelled and re-armed by a `CpuHotplugLevel::Timers` hook.) the balance round of `steal.test` with its off control; `hotplug.test` run with the period set. Repair items 22 and 23 are updated (22 is fixed when the setting is on, 23 gets a visit). Verified as step 1, plus the round passing with the period set and failing with it off. One commit.
3. **Back to the measurement.** W3 and W4 of `steal-policy-measure`, with "period 0 against 20 ms" as a configuration. Introduced separately.
Form A instead of step 2: the discount of the caller's weight in `steal_work_from()`, the flag per scheduler with `schedule.steal_busy` and a two-CPU round with a barrier (repair item 36 deleted), the worker thread with its event, a repair for item 38 or a decision to live with it, and the balance round.

## Files
- `kernel/lib/sched_domain/include/sched_domain/sched_domain.hpp`, `kernel/lib/sched_domain/test/`: step 1.
- `kernel/include/ours/boot/options.hpp`, `kernel/top/boot_options.cpp`, `kernel/kernel/task/sched/domains.cpp`, `kernel/commands/steal_policy.cpp`, `kernel/commands/steal_test.cpp` (the pin list), `kernel/tests/steal_policy_test.cpp`, `benches/kernel/sched/steal_bench.cpp`: step 1.
- `kernel/kernel/task/sched/scheduler.cpp` (`on_preemption_deadline()`), `kernel/include/ours/task/sched/scheduler.hpp` (the per-CPU `next_balance` and the counter), `kernel/commands/steal_test.cpp`: step 2. Form B would add `kernel/kernel/task/sched/balance_timer.cpp`, its `BUILD.gn` entry and a hook in `domains.hpp` for the knob's setter.
- `docs/changes/261103/kernel-task-sched.md`, `docs/repairs/261103/kernel-task-sched.md`, `.agents/skills/kernel-design/references/task.md`: each step.

## Test plan
Each step: `steal.test` ok on the default, Fair and four-node lanes; the 12 console tests; `ktest all`; `boot_check.py` on the three lanes; clang-tidy clean on the lines changed. Step 2 adds the balance round (a split falls to at most half its spread with the period set, and stays with it at 0) and `idle.test` unchanged with the setting off.
