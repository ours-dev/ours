# Performance report template

One report per performance topic, in `docs/perf/<YYMMDD>/<topic>.md`, where `<YYMMDD>` is the day the measurement started and `<topic>` names what is being made faster or smaller (`page-walk`, `tlb-shootdown`). It starts with the baseline, before anything changes, and gains one section per optimisation stage as each lands. Its raw samples sit beside it, in `docs/perf/<YYMMDD>/<topic>/`, one CSV file per stage: `baseline.csv`, `stage-1.csv`, and so on.

The report is for the user, who decides each stage (CLAUDE.md, "Working with the user"). It exists so that every number in a decision can be traced to the samples it came from, and so that each stage's gain is shown against the stage before, not only against the start.

Rules:

- **Raw samples, not summaries, are kept.** A benchmark writes one sample per row in the CSV format `scripts/perf_stats.py` reads: a header line `case,metric,unit,value`, then one row per sample. A sample is one measurement, such as one round's time per page. Lines starting with `#` are comments, for the machine, the build and the command.
- **Statistics come from the samples, through the script, never by hand.** Run `python3 scripts/perf_stats.py <file.csv>` for a stage's table: n, min, median, p90, max, mean and standard deviation of each case and metric. Run `python3 scripts/perf_stats.py --compare <before.csv> <after.csv>` for the gain of one stage over another. Paste the output as it is.
- **The median is the number a decision rests on**, with the spread (p90, max, standard deviation) beside it, so that a gain smaller than the noise is seen to be one.
- **A metric that needs the processor's caches or TLB** is measured on hardware, or marked as not a hardware number (`benches/kernel/include/bench/run.hpp`).

A section with nothing to say yet gets the word "None".

---

```markdown
# <topic>: performance report

**Date**: YYYY-MM-DD
**Status**: Measuring | Optimizing | Done

## Question
What is being made faster or smaller and why now: the repair items it answers, with their paths and numbers, and the user's decision that started it.

## Method
The machine and build, the benchmark and the exact command that produced the samples, what each metric means, and whether it needs hardware. How many samples each case has, and where the raw files are.

## Baseline
The statistics of the baseline samples (`scripts/perf_stats.py`), the file they came from, and what they say: where the time or the count goes, and what is noise.

## Stages
One `### Stage N: what changed` per optimisation stage, in order. Each gives:

- the commit;
- the user's decision and prediction;
- the statistics of its own samples;
- its gain over the baseline and over the stage before (`scripts/perf_stats.py --compare`);
- whether the prediction held.

## Conclusion
Where it stands: the total gain, what was not done and why, and what the numbers say about the next bottleneck.
```
