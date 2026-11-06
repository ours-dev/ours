# steal-policy-measure: how to measure what the work-stealing knobs do

**Date**: 2026-11-05
**Status**: Proposed

## Problem
The five numbers that steer work stealing (`enabled`, the two thresholds, the two batches) were chosen by reasoning, and repair item 24 of `docs/repairs/261103/kernel-task-sched.md` asks for data before they change. Since the knobs landed (`0b327369`) they can be set while the machine runs (`steal.policy`), so a sweep needs no rebuild. What is missing is the benchmark that runs the sweep and the method that makes its numbers mean something.

## Goals and non-goals
Goals: answer three questions with samples kept in `docs/perf/<YYMMDD>/steal-policy/` and a report in `docs/perf/<YYMMDD>/steal-policy.md`: whether stealing helps (on against off), how many threads a look should take and how deep a queue must be (batch 1, 2, 4, 8 and thresholds 2, 3, 4), and what it costs (migrations, the time a look holds a lock with interrupts off, the latency of threads that sleep and wake).

Non-goals: the value of preferring a CPU on the same node, which rests on remote-memory latency that QEMU's TCG does not model, so it waits for a multi-node machine; the load metric (the weight sum against a sampled, decayed load), which replaces `Scheduler::load()` and is its own item; the hotplug return of repair item 22.

## Design
**Machine.** QEMU TCG, `-cpu Nehalem`, 4 CPUs, as `docs/perf/261025/pick-cost.md`; the dbg image through `scripts/kshell_run.py`. A metric that rests on the processor's caches or TLB is `bench::Needs::Hardware` and is skipped unless the command is given `all`, and then it is marked as not a hardware number (`benches/kernel/include/bench/run.hpp`).

**Benchmark.** `steal.bench` (`benches/kernel/sched/steal_bench.cpp`, linked through `//benches/kernel:commands`), a kshell command that takes `[all] [csv]` like `preemptdeadline.bench`. It sweeps the configurations itself by calling `StealPolicy::set()` between rounds, so one boot gives many samples, and prints one raw row per sample (`case,metric,unit,value`) for `scripts/perf_stats.py`.

**Workloads.** Worker threads run a register-only loop, so their speed does not depend on a cache; a controller thread pinned to CPU 0 starts and watches them on CPUs 1 to 3.

| Workload | What it does | Metrics | Needs |
|---|---|---|---|
| W1 balance | 8 workers pinned to CPU 1, then allowed on CPUs 1 to 3 | when the last worker changed CPU, and whether nothing moved for the last 50 ms of the 2 s window (settled); threads moved (`schedule.steals`); workers per CPU at the end, as the spread (most less fewest); the slowest worker's progress over the mean | any machine |
| W2 throughput | as W1, the same 2 s window | total iterations of all workers, over the iterations of one worker alone on one CPU in the same time (measured before each round): the effective number of CPUs | any machine |
| W3 latency | one worker on each of CPUs 1 to 3 and 6 threads that sleep 5 ms and run 100 us | time from wake to run, p50 and p99; threads moved | relative values on any machine |
| W4 cost | during W1 | time inside `steal_work_from()` per thief: the sum and the longest (`schedule.steal_ns`, `schedule.steal_max_ns`) | absolute values need hardware; counts any machine |

**Sweep.** Fifteen configurations, seven on eight workers and eight on a shallow queue. On eight workers: stealing off; the defaults (`min-runnable-same-node` 2, `min-runnable-other-node` 3, both batches 4); the batch at 1, 2 and 8 with the thresholds at their defaults; the same-node threshold at 3 and at 4 with the batches at their defaults. The thresholds show only on a queue of about their depth (eight workers queue deeper than any of them), so on three workers and on four there are off, the defaults and the threshold at 3 and at 4 (the batch cannot show there: a look takes the one thread the mean leaves room for). A batch of 1 stands for the old behaviour of one thread per look. Thirty samples a configuration and workload: five boots of six rounds, the first round of a boot dropped as warm-up, the configurations interleaved so that a drift of the host falls on all of them.

**Statistics.** From the raw rows through `scripts/perf_stats.py` only: n, min, median, p90, max, mean, standard deviation, and `--compare` for a gain. A difference smaller than the spread is reported as noise.

**Why W1 is settled, not balanced.** An idle CPU looks once, when something wakes it (repair item 23), and the mean it reaches for counts every CPU of its tier, the controller's CPU included, which cannot hold a worker. With 8 workers on CPU 1 and the defaults, CPU 2 takes 2 and then CPU 3 takes 2: the end is 4/2/2, a spread of 2, and no one looks again. A criterion of "at most one apart" would time out in every configuration, off included, and say nothing. So W1 reports where the workers are when nothing has moved for 50 ms, and how fair that is. The user decided it on 2026-11-06.

**Validity.** The off configuration must show no balance (W1 ends 8/0/0 with nothing moved, and W2 is close to 1 CPU): a harness that cannot see the absence of stealing cannot see its presence. The host's load average goes into the comment line of every batch of samples, and a batch taken while another process held a core is discarded and taken again (a `qemu` of another worktree already made `preempt.test` fail that way on 2026-11-05).

**The counters.** `steal_work_from()` times each look with the steady clock and adds the nanoseconds to `schedule.steal_ns` and the longest to `schedule.steal_max_ns`, per thief, whichever CPU made the call. They cost two clock reads on a look (looks of a disabled policy return before the first), and what they report includes about one clock read. The call holds preemption off, so a caller that may be preempted does not add the time it waited to run to the figures.

## Alternatives considered
- Measuring on a real machine first: none is at hand, and the algorithm-level questions do not need one.
- One boot per configuration: ten times the wall time for the same samples, with boot-to-boot noise mixed into the comparison.
- Timing a look from outside (the bench around `steal_work_from()`): the call can be made on any CPU's scheduler since 2026-11-06, but a time taken around it holds the caller's own clock reads and, from another CPU, the cost of reaching the scheduler; the counters measure the look itself, with preemption off.
- A tracer for the time in `steal_work_from()`: a sampling profile cannot see a section of a few microseconds, and the counters are permanent and cheap.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Scope of the first round | QEMU only; wait for a multi-node machine | QEMU only; same-node versus other-node waits for hardware | None (predictions are paused) | QEMU only, 2026-11-05 |
| A pair of permanent counters for the time of a look | add; do not add | add | None | add, 2026-11-05 |
| Workloads of the first round | W1, W2, W3, W4; also the hotplug return | W1 to W4 | None | W1 to W4, 2026-11-05 |
| W1's criterion | settled (nothing moved for 50 ms); at most one apart | settled | None (predictions are paused) | settled, 2026-11-06 |

## Testing and verification
The benchmark is checked against its own negative control (off), by running it with a knob that must change a result, and by the host-load rule above. The counters are checked by the unit that reads them (`steal.test`). Nothing here can verify the same-node preference: that needs hardware.

## First run
Two rounds on the default lane (`steal.bench rounds=2 csv`, 2026-11-06), as a check of the harness, not as data: off ended 8/0/0 with 1.0 CPUs and nothing moved; the defaults 4/2/2 with 2.9 CPUs and 4 moved; a batch of 1 gave 6/1/1 and 2 moved; batches of 2 and 8 gave 4/2/2 as the defaults. The thresholds (`min3`, `min4`) gave the same as the defaults: eight workers queue on one CPU, deeper than any threshold swept, so this workload cannot tell them apart. A threshold needs a queue of about its own depth (3 or 4 workers on the victim); that is for W3 or a shallow variant of W1, and is a question below. W2's CPUs barely change between configurations that keep three CPUs busy, as expected: what differs is who has a CPU to themselves, which `w1 slowest` shows.

**What the harness cannot tell, found by review.** `w1 slowest` is quantised by the 64 ms slice against a 2 s window: eight workers on one CPU read 0.8 to 1, so only a large difference (6/1/1 against 4/2/2) stands clear of it. `w2 cpus` divides three CPUs' work by one worker's alone, so an emulator that runs three CPUs at once beside the controller's reads about 2.9 where a perfect balance would be 3.0; off against on is not understated by more than that 3%, and the comparison between configurations is unaffected. The workers' records share cache lines, which only a real processor would charge. A default run is seven rounds, the first a warm-up, so six rows of each metric and configuration a boot and thirty over five boots; it takes 284 s of waiting and needs `kshell_run.py --command-timeout 600`.

**The shallow cases** (two rounds, default lane, 2026-11-06) ended as the mean rule predicts: three workers, off 3/0/0, the defaults one on each CPU, `min3` 2/1/0 (the second look finds 2 runnable, under 3), `min4` 3/0/0; four workers, off 4/0/0, the defaults and `min3` 2/1/1, `min4` 3/1/0. The thresholds are told apart there, in the number of CPUs kept busy (3.0, 2.0 and 1.0 for three workers).

## Open questions
- Whether 30 samples a configuration are enough to see a batch of 4 against a batch of 8; the first run decides, from the spread.
- Whether the effective number of CPUs of W2 is stable under the host load that a second QEMU on the same machine causes.
