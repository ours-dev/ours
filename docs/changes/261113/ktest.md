# ktest changes

## The environment walk and the run plan lose their C shapes (2026-11-13)

**Files**

- `kernel/lib/ktest/src/run_stage.hpp` — new: the shared stage runner (`run_hook`) and the published run state (`CURRENT_STATE`) as a header-inline pair, compiled once by the runner, the environment and the host suite alike.
- `kernel/lib/ktest/include/ktest/run_state.hpp` — declares the stage-thread half of the seam: `bind_stage_thread()` / `unbind_stage_thread()`, which the kernel's runner implements over the runner thread and the host's stand-in keeps fixed.
- `kernel/lib/ktest/include/ktest/environment.hpp` — `Environment` gains the two stage virtuals (`set_up_stage`, `tear_down_stage`, each over `hsien::Reporter &`); `internal::EnvironmentStep`, the C function pointer plus `void *context`, is gone, and `EnvironmentRun::set_up` / `tear_down` take only the reporter.
- `kernel/lib/ktest/src/environment.cpp` — the default stages frame `SetUp()` / `TearDown()` through the shared stage runner, and the walk calls each environment's stage itself; a failed TearDown reports through the framework-error channel, the same one line the runner used to log.
- `kernel/lib/ktest/src/runner.cpp` — drops its private publish state, its `run_hook`, the `EnvironmentFraming` struct and `run_environment_step`; the option loop becomes one `RunPlan::from_words` call plus a rejection-to-line switch; `option_usize` goes through the shared digit primitive; the option table becomes the run's plan.
- `kernel/lib/ktest/include/ktest/run_plan.hpp` — `parse_u64(sv, base) -> Result<u64>` folds the three hand-written digit loops; `Shard` with `Shard::parse` absorbs `parse_shard` + `shard_valid`; `shuffle` takes a span; `RunPlan::from_words` absorbs the runner's option walk and validation and records every word; `PlanFault`/`PlanRejection` name what broke. `parse_decimal`, `parse_shard`, `shard_valid` and `shuffle_indices` are gone.
- `kernel/lib/ktest/test/plan_test.cpp` — the parsing cases move to the new shapes with their verdicts kept, and ten new cases walk `from_words`' accept and reject paths.
- `kernel/tests/ktest_fixture_test.cpp` — the frame's own smoke derives its two order claims from which of its suite mates actually ran ahead of it, instead of from the section order: under a shuffled run the suite runs in another order, and the claims hold there too. The section-order run asserts exactly what it asserted before.
- `kernel/lib/ktest/test/fixture_test.cpp` — the environment cases drive `run.set_up(REPORTER)` / `run.tear_down(REPORTER)`; the fake step function pointer is gone and a broken SetUp fails the way a real one does, by recording a failed check; a new case pins the failed-teardown report and the unwind that continues past it.
- `kernel/lib/ktest/test/host_shim.hpp`, `kernel/lib/ktest/test/host_shim.cpp` — the stand-in publishes the shared stage runner's state instead of a private slot, and stands in for the two new seam functions.
- `kernel/tests/ktest_test.cpp` — the three runner-option smoke tests read the seed and the shard through `parse_seed` / `Shard::parse`'s `Result` shapes.

**Symptom**

Latent, cannot be triggered today: three review findings against the features merge's API shapes, all of them working code. The environment walk was a C callback boundary — `EnvironmentStep`, a function pointer plus a `void *context` that every caller had to reinterpret; the run plan parsed into out-parameters (`parse_shard(value, m, n)`, `parse_seed(value, out)`) and its shuffle filled a raw `usize *` plus a count; and the digit loop that parsed them was written out three times (decimal, seed, `option_usize`), each with its own drift risk.

**Cause**

The walk was shaped after the C idiom because the framework to frame a hook lived in runner.cpp's anonymous namespace: the published state, the owner thread and the binding scope were the runner's private machinery, so environment.cpp could not reach them and the only channel left was a function pointer the runner handed back into itself. That shape pushed the invariant outward — which half of the stage was running was a bool smuggled through a `void *` (the `EnvironmentFraming` struct), and the caller who wrote that bool also had to remember to flip it between the two walks. The plan parsers took out-params because they predated the codebase's `Result` convention being applied to ktest, and each kept its own digit loop because no primitive sat beneath them: three loops that agree today and drift the first time someone fixes overflow handling in one of them.

**Change**

The walk now frames each stage itself. The hook-binding machinery moved into `src/run_stage.hpp`, header-inline so the kernel image and the host suite compile the same definition; `Environment` owns its framing as two virtuals, and the walk is two plain calls:

```cpp
// before: the caller framed every step and smuggled the half through a void *
struct EnvironmentFraming { hsien::Reporter *reporter; bool setup; };
auto step = auto (*)(Environment &environment, void *context) -> bool;   // EnvironmentStep
environment_run.set_up(run_environment_step, &framing);
framing.setup = false;
environment_run.tear_down(run_environment_step, &framing);

// after: the environment frames its own stage; the walk asks each object
class Environment {
    virtual auto set_up_stage(hsien::Reporter &reporter) -> bool;      // frames SetUp()
    virtual auto tear_down_stage(hsien::Reporter &reporter) -> void;   // frames TearDown(), reports a failure
};
environment_run.set_up(reporter);
environment_run.tear_down(reporter);
```

```cpp
// before: out-params and a raw pointer plus a count
constexpr auto parse_decimal(std::string_view value, usize &out) -> bool;
constexpr auto parse_seed(std::string_view value, u64 &out) -> bool;
constexpr auto parse_shard(std::string_view value, usize &m, usize &n) -> bool;
constexpr auto shard_valid(usize m, usize n) -> bool;
constexpr auto shuffle_indices(usize *indices, usize count, u64 seed) -> void;

// after: one digit primitive, one shard type, a span, and one builder
constexpr auto parse_u64(std::string_view digits, u64 base = 10) -> ustl::Result<u64, ours::Status>;
constexpr auto parse_seed(std::string_view value) -> ustl::Result<u64, ours::Status>;
struct Shard { usize m; usize n; static auto parse(std::string_view) -> Result<Shard>; };
constexpr auto shuffle(std::span<usize> indices, u64 seed) -> void;
constexpr auto RunPlan::from_words(std::span<std::string_view const> words, bool runner_options_allowed)
    -> ustl::Result<RunPlan, PlanRejection>;
```

`from_words` records every word the run carried — the runner's own three set the fields on the way through — so the runner's option table, its parse loop and its interpret loop became one call followed by a switch that maps each `PlanFault` to the exact console line it always printed. The `runner_options_allowed` parameter carries the one fact the header cannot derive for itself: that the runner's own options belong to `ktest all` alone. One trap the first boot run caught: passing the runner's fixed `std::string_view words[MAX_RUN_OPTIONS]` array converts to a span of the array's extent, so a bare `ktest list` would have parsed fourteen empty words and died at the usage line; the span must be built with the word count, and a `from_words` case on an empty span now pins it.

**Reference**

GoogleTest's `testing::Environment` (`vendor/googletest/googletest/include/gtest/gtest.h`, run through `TestEnvironments` in `googletest/src/gtest.cc`) — global environments are objects with virtual `SetUp`/`TearDown`, added to the runner, and the runner owns the framing; the stage virtuals here give the same shape a seam the host can also compile. The plan side follows Rust's `str::parse::<u64>() -> Result<u64, ParseIntError>` for the digit primitive and `slice::shuffle` (a Fisher–Yates over the slice) for the span; the rejection type mirrors Rust's `ParseIntError`-carrying `Result`, with the console line the one thing the error payload names.

**Verification**

- `boot_check.py --out out/bringup.x86`: build ok; dbg PASS 46/5/0; rel PASS 44/8/0 (7/7 required); host-tests 183/0.
- Host suite: `ktest-test` 83/83 (73 before, ten `from_words` cases and the failed-teardown case added, every old case's verdict kept).
- `python3 -I scripts/test_kshell_run.py`: 36/36 — the verdict grammar untouched.
- `ktest all` on the dbg image: `ktest: SUMMARY ran=79 passed=77 failed=0 skipped=2` — the two known skips unchanged.
- `ktest all shuffle repeat=2 shard=0/1`: `ktest: SHUFFLE seed=0x243f6a8885a308d3 order=69,1,65,...` — the full 79-index line — then `SUMMARY ran=158 passed=154 failed=0 skipped=4` — the three options compose, twice the plain run, all four skips the two known skips seen twice. This run is also the repair's own gate: at the tip it panicked in logz4's allocating format buffer (see `261113/kernel-log.md`, fixed before this change was committed).
- cov_host on `kernel/lib/ktest`: run_plan.hpp 142/142, run_state.hpp 6/6, src/run_stage.hpp 11/11, src/environment.cpp 48/48 — the touched headers all at 100% lines, above the 95% gate.
- clang-format clean on every touched file; clang-tidy reports nothing on the changed lines (its remaining output names pre-existing diagnostics: the four linker-script externs in runner.cpp and arch/vendor headers).

**Deliberately not done**

- Making the frame's smoke's order claims absolute. `KtestFixture.GetsAFreshSetUp`'s `FIXTURE_TEARDOWNS == 2` and `ReadsWhatSetUpWrote`'s `== 0` asserted the section order itself, and the first composition run failed them twice: under a shuffled run the suite runs in another order, and the framework's own shuffle contract (GoogleTest's `--gtest_shuffle` demands order independence) had no exception to offer. The claims now derive from which suite mates actually ran ahead — the section-order run asserts what it always asserted, and the shuffled run asserts the same interleaving in the order it actually ran. Making them absolute instead — dropping the order proof from the frame's own smoke — would have given up the property the suite exists to prove.
- Moving the owner thread into `run_stage.hpp`. Its slot is `std::atomic<ours::task::Thread *>`, and naming that type pulls the kernel's thread header into a header the host suite must compile; the seam pair (`bind_stage_thread`/`unbind_stage_thread`) keeps the publish in the stage runner and the type in the one file that can name it.
- Unifying the host shim's `bind_context`/`record_skip` null-state paths with the kernel's. The kernel reports a framework error there; the stand-in keeps its silent shape, because a unified body would sit in a shared header with its error lines unreached by any well-behaved host case, and the 95% gate on touched headers is part of this change's contract.
- A distinct error per shard fault. Malformed and out-of-range map to one console line and always have; `Shard::parse` keeps both shapes as errors but pays for no distinction no caller reads.
- An overflow tolerance in `option_usize`. The old hand-written loop wrapped silently past u64; going through `parse_u64` such a value is now reported as "is not a number" and the fallback used. Nothing passes a value that large today, and the report is the honest verdict of what the parse saw.
- The `ktest all` walk's remaining registry-side checks (the shuffle bound, the dropped-environment report) stayed in the runner: they read the linked image, which the plan's pure-string world cannot see.

**How it was found**

None (review-driven refactor of the features merge, all three findings named in its review).

**The user's prediction**

None (mechanical refactor; behavior pinned by the host suite and the console grammar).

**Three questions you should be able to answer**

- Why does the default `set_up_stage` answer from the hook's failure count instead of from a flag the walk sets?
- What makes `EnvironmentRun::tear_down` unwind the attempted prefix rather than the whole registry, and why does a failed TearDown not stop it?
- `from_words` takes a bool for the `ktest all` rule — what makes that fact un-derivable from the words themselves, and what breaks if the runner stopped passing it?
