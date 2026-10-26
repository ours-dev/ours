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
