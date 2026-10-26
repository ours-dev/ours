# scripts changes

## Title: performance reports: a template, a statistics script, and lint_docs checks for docs/perf (2026-10-25)

**Files**
- `docs/perf/TEMPLATE.md` — new: the performance report, its rules, and its skeleton (Question, Method, Baseline, Stages, Conclusion)
- `scripts/perf_stats.py` — new: statistics of a benchmark's raw samples (CSV `case,metric,unit,value`), and the gain of one stage over another
- `scripts/lint_docs.py` — `docs/perf/<YYMMDD>/<topic>.md` is a document family. lint_docs checks its path, its H1 (`<topic>: `), its Date and Status (Measuring, Optimizing, Done), and its sections in the template's order.
- `CLAUDE.md`, `AGENTS.md` — the rule, in "Working with the user", "Documentation" and "Formatting"

**Symptom**
None, a new standard. The user asked on 2026-10-25 that every performance item get its own report, recording the statistics of the raw samples and the gain of each optimisation stage.

**Cause**
Until now a measurement lived in a repair item or a change record as a pasted table. Two things were missing:

- **Raw samples were not kept.** A number could not be traced back to them.
- **Gains had no fixed place.** A gain was shown against the start, if at all, never stage by stage.

**Change**
Objects:

- **A report per topic**, in `docs/perf/<YYMMDD>/<topic>.md`. It starts at the baseline and gains a `### Stage N` per optimisation.
- **Its raw samples beside it**, in `docs/perf/<YYMMDD>/<topic>/`, one CSV file per stage, one sample per row.
- **`perf_stats.py`**, which turns a CSV into the statistics table (n, min, median, p90, max, mean, sample standard deviation), and two CSVs into the gain table: median before and after, their ratio, and the change in percent. A ratio within either run's spread (p90 minus median) is marked "within noise".
- **lint_docs**, which holds a report to the template as it does task documents.

The median is the number a decision rests on, with the spread beside it. A metric that needs the processor's caches or TLB follows `bench::Run`'s convention.

**Reference**
None, this project's own convention.

**Verification**
- `perf_stats.py` on a sample CSV gives the expected statistics (median 1.20, p90 1.36 of 1.0, 1.2, 1.4), and on two files the gain (1.20 to 0.55: 2.18x, -54.2%).
- lint_docs on a malformed report: a wrong title, a task status, and three missing sections, each reported. The template and both instruction files are clean, with no line over 80 columns in the two.

**Deliberately not done**
- Writing CSV from the in-kernel benchmarks (`largepage.bench`): each will, when it next feeds a report.

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- Why are the raw samples kept, and not only the statistics?
- Why does a decision rest on the median and not the mean?
- When is a stage's gain "within noise", and what should be done about one that is?

## Title: flame graphs: flamegraph.py records a host process with /usr/bin/sample, folds its stacks and renders them; page-walk's first three (2026-10-25)

**Files**
- `scripts/flamegraph.py` — new:
  - `record`: start a command, wait for its `ready`, sample it, write folded stacks;
  - `fold`: a `/usr/bin/sample` report to folded stacks;
  - `render`: folded stacks to a self-contained page with hover, zoom and search.
- `benches/kernel/lib/arch/page_walk_bench.cpp` — two changes:
  - `spin SECONDS CASE` repeats one case for a profiler, after printing `ready`;
  - each engine call goes through a wrapper that is never inlined (`engine_map()`, `engine_protect()`, `engine_unmap()`).
- `docs/perf/TEMPLATE.md`, `CLAUDE.md`, `AGENTS.md` — a stage keeps its folded stacks and flame graphs where a profiler can sample it
- `docs/perf/261025/page-walk.md` and `page-walk/` — the baseline re-recorded with the wrappers; `baseline-inlined.csv` keeps the first recording; three folded stacks and their pages; the findings
- `docs/repairs/261025/arch-paging.md` — item 2 carries the out-of-line cost; item 7 is new: `split_ends()`

**Symptom**
The user could not judge the walks from the statistics alone, and asked for flame graphs (2026-10-25).

**Cause**
None, a new tool. Three things had to be solved for a flame graph of the bench to say anything:

- **When to attach.** `/usr/bin/sample` attached right after the launch caught the process held in the loader: 291 of 296 samples sat on `_dyld_start`, waiting on the profiler's own load notification. The bench now prints `ready` from `main()`, and `record` attaches on that line.
- **One case at a time.** All cases in one process make one tree whose frames cannot tell a one-page call from a 32768-page one. `spin` takes the case.
- **Seeing the operations.** Inlined, map and protect were `run()` itself. Wrappers that are never inlined split the three, and the engine is still inlined into each, so the profile keeps the shape the compiler gives it.

**Change**
- **The folded file is the raw data and is kept whole.** Labels are cut down only on the page: no namespaces or parameter lists, templates cut to a paging level (`Paging::split_at<L3>`).
- **The wrappers change the numbers.** With the options a run-time argument behind an opaque call, as in the kernel's `PageTable::map_pages()`, the compiler can no longer fold the per-call setup away:
  - a one-page map rose from 1.68 to 2.62 ns;
  - a bulk map from 1.10 to 3.27 ns a page;
  - the 32768-page cases did not move.

  The baseline was re-recorded with them, so that its statistics and its flame graphs come from one binary. The earlier samples are kept for the comparison.

**Reference**
The folded format and the flame graph are Brendan Gregg's (`stackcollapse` and `flamegraph.pl`). This is a Python rendering of the same idea, as the repo's tooling is Python.

**Verification**
- `record` on the three cases gave 3834 to 3863 samples each over 5 seconds, every one under `main()`. Folding keeps the total: 4205 of 4205 on the first try.
- The pages render, zoom and search (`split_at` matches 34.0 % of the `4k-1` graph).
- `lint_docs` passes the report, the template, the repair items and both instruction files.

**Deliberately not done**
- A view inside the engine's walk (levels, `locate_entry`): it is inlined, and a build without inlining would profile a different program. The counts already say how many tables each case reads.
- Profiling inside the kernel: the sampler needs a program holding the SAMPLING resource, and no workload drives the page table from one yet.

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- Why did the first recording show almost nothing but `_dyld_start`, and how does `ready` fix it?
- Why did wrapping the engine calls make the one-page and bulk cases slower but leave the large ones alone?
- What share of a one-page protect is `split_ends()`, and why does it vanish at 32768 pages?
