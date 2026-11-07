# ktest changes

## shuffle, repeat and shard as runner options of `ktest all` (2026-11-07)

**Files**

- `kernel/lib/ktest/include/ktest/run_plan.hpp` — new: the run plan (`RunPlan`, `RunnerOption`), the value parsers (`parse_decimal`, `parse_seed`, `parse_shard`, `shard_valid`), splitmix64's `next_random`, Fisher-Yates' `shuffle_indices` and the `in_shard` predicate. Pure `string_view`/`usize` arithmetic with no kernel headers, so the host tests exercise the same parsing and selection math the runner runs.
- `kernel/lib/ktest/src/runner.cpp` — parses the three runner options into a `RunPlan` after the generic option parse, rejects them on a filter run, shuffles once and logs the `SHUFFLE` line, and iterates `repeat` passes over the shard-filtered order.
- `kernel/lib/ktest/test/plan_test.cpp` — new host tests: option-name dispatch, seed/decimal/shard parsing, the shard range rule, the partition property, shuffle determinism and permutation, tiny registries.
- `kernel/lib/ktest/test/summary_test.cpp` — a case for counting under repeat: every execution records, so one test can contribute a skip in one pass and a pass in another.
- `kernel/lib/ktest/BUILD.gn`, `kernel/lib/ktest/test/BUILD.gn` — wire the new header and host test file into the headers interface and `ktest-test`.
- `kernel/tests/ktest_test.cpp` — three booted smoke tests (`Ktest.ReadsShuffleOption`, `Ktest.ReadsRepeatOption`, `Ktest.ReadsShardOption`) that read the runner's options through the same accessors `Ktest.ReadsRunOptions` uses, each silent unless its option was given.
- `scripts/kshell_run.py` — a run carrying `repeat=` or `shard=` skips the `ktest list` setup and the `ran == listed` equality (`ktest_count_varies`); a shuffle-only run keeps both; `failed=0` and the one-summary rule stay for every run.
- `scripts/test_kshell_run.py` — cases for the helper and for repeat- and shard-shaped summaries judged with no expected count.
- `docs/design/261105/kernel-test-framework.md` — the grammar, the run-options paragraph and the non-goals line.

**Symptom**

Not a defect: the runner had exactly one iteration shape — section order, once — so a selection could only ever be seen in one order and one count. An order-dependent interaction between two tests, and the question of what a pass count does to a summary, had no way to surface themselves; and there was no way to split the registry across two machines.

**Cause**

The runner's loop was written as the single `for` over the record section that the design's "Events of one `ktest <selection>`" specified, and everything else about a run (the host judge included) grew around that shape. The host judge's only completeness check was `ran == listed` — precisely the invariant that repeat and shard break — so adding the options without touching the judge would have made every optioned run fail the host verdict, or, fixed by skipping the check for any option, would have silently dropped the check for runs (a bare `shuffle`, `rounds=256`) where it still says something. The iteration shape and the judge had to move together.

**Change**

`ktest all` now takes three runner options, parsed like the existing `name=value` tokens but interpreted by the runner instead of being handed to the tests:

- `shuffle[=SEED]` — runs the selection in a Fisher-Yates order drawn from a splitmix64 sequence seeded by `SEED` (hex with an `0x`/`0X` prefix, decimal without; a fixed default, `0x243F6A8885A308D3`, when bare). The runner logs `ktest: SHUFFLE seed=0x... order=...` once, the order being the registry indices that will run, in run order — the section positions `ktest list` prints.
- `repeat=N` — runs the whole selection `N` times; the `SUMMARY` counts every execution. `repeat=0` is a malformed run (`ktest: ERROR`, no summary), because a run that reports `ran=0` already has a meaning — nothing matched.
- `shard=M/N` — runs only the tests whose registry index `i` satisfies `i % N == M` (0-based, `N > 0`, `M < N`, else `ERROR`). The SUMMARY reflects the shard.

The three judgments worth recording:

- Runner options are `all`-only: a filter run carrying any of them is an ERROR. The task specified this for shuffle; applying it to all three keeps one rule instead of three ("runner options belong to `ktest all`").
- The shard predicate runs on the registry index, not on the shuffled position, so a shard's membership never depends on the seed it is shuffled with; the shuffle then orders the shard's tests. Sharding that changed membership with the seed could not divide work between two machines.
- One shuffle per run, not per pass: `shuffle repeat=2` runs the same shuffled order twice, which is what the one `SHUFFLE` line reports. Reshuffling per pass would need one line per pass and would make the pass count part of the order's identity for no use the batch asked for.

The run loop, before and after:

```cpp
for (auto const *record = begin; record != end; ++record) {
    if (!matches_filter(record->name, filter)) {
        continue;
    }
    run_test(*record, reporter, summary);
}
```

```cpp
for (auto pass = usize{0}; pass < plan.repeat; ++pass) {
    for (auto pos = usize{0}; pos < count; ++pos) {
        auto index = pos;
        if (plan.shuffle) {
            index = SHUFFLE_ORDER[pos];
        }
        auto const &record = begin[index];
        if (!matches_filter(record.name, filter) ||
            !in_shard(index, plan.shard_index, plan.shard_count)) {
            continue;
        }
        run_test(record, reporter, summary);
    }
}
```

The permutation lives in one fixed `usize SHUFFLE_ORDER[MAX_SHUFFLE_TESTS]` (256) next to the existing option table, filled only by a run that asked for a shuffle and bounded by an explicit error beyond it — the re-entry guard makes one run at a time the only writer, which is what lets a fixed array stand where a heap vector would. The `SHUFFLE` line's order text is appended into a fixed buffer sized for the worst plan.

On the host side, `kshell_run.py` learned one helper, `ktest_count_varies`: a `ktest all` command carrying `repeat=` or `shard=` skips the `ktest list` setup and the count equality, and everything else (one summary, counts that add up, `failed=0`, `ran > 0`, no error lines) still applies to every run. Shuffle keeps the count check, because a reordering does not change `ran`.

**Reference**

GoogleTest's run-shaping flags: `--gtest_repeat`, `--gtest_shuffle` with `--gtest_random_seed` (`vendor/googletest/googletest/src/gtest.cc`, `GTEST_FLAG(repeat)` and friends), and sharding through the `GTEST_TOTAL_SHARDS`/`GTEST_SHARD_INDEX` environment pair, which also shards by index so a shard's membership does not depend on the seed. The option grammar itself (one `name=value` token, a bare name for a presence-only flag) is this framework's own, from the run-options step of 2026-11-05. Zircon's `k ut` has none of these — its runner (`zircon/kernel/lib/unittest/console.rs`) walks the cases in registration order and stops.

**Verification**

- Host: `python3 -I scripts/test_kshell_run.py` 21/21; `boot_check.py --out out/ktest-batch1` — build ok, dbg PASS 46/5/0, rel PASS 44/8/0 (7/7 required), host-tests 183/0, the 12 new `KtestRunPlan` cases and the repeat-counting case among them.
- `ktest list` 64 tests; plain `ktest all` on the dbg image: `ktest: SUMMARY ran=64 passed=62 failed=0 skipped=2` — the registry is 61 migrated tests plus the three new smoke tests, and the two known skips (Preempt's re-measurement, Ktest.SkipIsReported) are unchanged.
- `ktest all shuffle repeat=2`: `ktest: SHUFFLE seed=0x243f6a8885a308d3 order=22,29,9,50,...`, then `ktest: SUMMARY ran=128 passed=124 failed=0 skipped=4` — exactly twice the plain run, all four skips being the two known skips seen twice.
- `ktest all shard=1/2`: `ktest: SUMMARY ran=32 passed=31 failed=0 skipped=1` — the odd half of 64, with whichever known skip falls on an odd index.
- Determinism: `ktest all shuffle=305419896 seed=305419896` on two separate boots produced byte-identical `SHUFFLE seed=0x0000000012345678 order=54,28,50,2,...` lines, and the shuffle-only run kept the host's list-count check (`ktest list (setup): ok, 64 test(s)`, `ok, 64 ran`).
- Malformed runs each end at their error line without a summary, judged FAIL by the host: `ktest Ktest.* shuffle` → `ktest: ERROR shuffle, repeat and shard need "ktest all"`; `ktest all repeat=0` → `ktest: ERROR repeat is not a count of at least 1: "0"`; `ktest all shard=2/2` → `ktest: ERROR shard is not M/N with 0 <= M < N: "2/2"`.
- clang-format clean on every touched file; clang-tidy reports nothing on the changed lines (its remaining output names pre-existing diagnostics in arch and vendor headers); `format_gn.py` clean; `lint_docs.py` clean on this record and the design doc.

**Deliberately not done**

- Reshuffling per repeat pass: one shuffle per run, one `SHUFFLE` line.
- Sharding by shuffled position: it would make membership seed-dependent.
- Runner options with a filter: an error, for one uniform rule.
- A cap on `repeat`: a runaway count hangs the shell exactly like any long run, and the host's silence NMI is the diagnosis; the option's meaning, not the runner, bounds it.
- Keeping the per-test options path untouched: `shuffle`/`repeat`/`shard` remain visible to tests through `has_option`/`option` like any other token, so a test that wanted an option with one of those names would collide; no current test does, and the smoke tests pair with `expected_<option>` names instead of the generic `expected` for exactly this reason (the first booted run of `repeat=2 expected=2` failed `Ktest.ReadsRunOptions`, which owns `expected` and demands 42).

**How it was found**

None (feature).

**The user's prediction**

None.

**Three questions you should be able to answer**

- Why does the shard predicate run on the registry index and not on the shuffled position?
- What does the host judge still check about a repeat or shard run's summary, and what does it stop checking?
- Why is `repeat=0` an error rather than a legitimate run whose summary says `ran=0`?
