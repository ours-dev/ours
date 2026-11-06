# steal-policy-measure: steps to measure the work-stealing knobs

**Date**: 2026-11-05
**Status**: Proposed

## Request
The user, 2026-11-05: design the measurement of the work-stealing knobs (`设计测量方法`), then "按推荐定" for its three decisions. Scope: the first round on QEMU, W1 to W4, with the look-duration counters. Ends at the report; changing a default is a separate decision that the report informs.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| See DESIGN.md | | | | 2026-11-05 |

## Steps
1. **The counters and this task's documents.** `schedule.steal_ns` and `schedule.steal_max_ns` in `steal()`, their accessors, a line in `steal.policy`, a check in `steal.test`. Verified with the three lanes and `boot_check.py`; one commit.
2. **`steal.bench` with W1 and W2.** The workers, the controller, the sampling of workers per CPU, the CSV rows; the off configuration as the negative control. Verified by running it and reading the rows; one commit.
3. **W3 and W4.** The sleeping threads and the latency samples; the cost rows from the counters. One commit.
4. **The run and the report.** The sweep, the raw samples in `docs/perf/<YYMMDD>/steal-policy/`, the statistics through `scripts/perf_stats.py`, the report from `docs/perf/TEMPLATE.md`. The user reads it and decides each stage.

## Files
- `kernel/kernel/task/sched/scheduler.cpp`, `kernel/include/ours/task/sched/scheduler.hpp`, `kernel/commands/steal_policy.cpp`, `kernel/commands/steal_test.cpp`: step 1.
- `benches/kernel/sched/steal_bench.cpp`, its `BUILD.gn`, `benches/kernel/BUILD.gn`: steps 2 and 3.
- `docs/perf/<YYMMDD>/steal-policy.md` and `steal-policy/*.csv`: step 4.

## Test plan
`steal.test` ok on the default, Fair and four-node lanes; `ktest all`; `boot_check.py` on the three; `steal.bench` prints rows for every case, and the off configuration shows no balance.
