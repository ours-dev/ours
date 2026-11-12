/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The `ktest` console command: walks the runtime's record section, applies a
// selection and a run plan (shuffle, repeat, shard), and runs each selected
// test through the runtime one at a time so it can frame and time it. Its
// lines are the host's contract: `RUN`, `OK`, `FAIL`, `FAILED`, `SKIP`,
// `TEST`, `ERROR`, the `SHUFFLE` line a shuffled run opens with, and the
// final `SUMMARY`.
#include <ktest/environment.hpp>
#include <ktest/filter.hpp>
#include <ktest/fixture.hpp>
#include <ktest/options.hpp>
#include <ktest/run_plan.hpp>
#include <ktest/run_state.hpp>
#include <ktest/summary.hpp>

#include <atomic>
#include <string_view>

#include <gktl/command.hpp>
#include <hsien/section.hpp>
#include <logz4/log.hpp>
#include <ours/status.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

namespace ktest {

/// The suite hook section (kernel/lib/ktest's SuiteHook, emitted by
/// KTEST_SUITE_SETUP and KTEST_SUITE_TEARDOWN), bounded by the linker script
/// the way the hsien records are. Read on demand, so it lives in
/// `.data.rel.ro` and not in the .init tail.
extern "C" internal::SuiteHook ktest_suite_hooks_start[] asm("__ktest_suite_hooks_start");
extern "C" internal::SuiteHook ktest_suite_hooks_end[] asm("__ktest_suite_hooks_end");

/// The death-test section (kernel/lib/ktest's KTEST_DEATH), the same
/// hsien::TestRecord as the live tests but in a section of its own: the
/// live walk below never touches it, so `ktest all` and a filter run
/// cannot run a death test, and only the `death` selection reads here.
extern "C" ::hsien::TestRecord ktest_death_start[] asm("__ktest_death_start");
extern "C" ::hsien::TestRecord ktest_death_end[] asm("__ktest_death_end");

namespace {

    /// The test the runner is executing, published for the adapter and the
    /// assertion macros. Only the runner thread writes it, but a worker thread
    /// that asserts by mistake reads it, so it is atomic: the read that is
    /// there to diagnose the mistake must not race the store.
    std::atomic<internal::RunState *> CURRENT_STATE{nullptr};

    /// The thread the published test runs on, for the wrong-thread guard. Same
    /// reason as CURRENT_STATE.
    std::atomic<::ours::task::Thread *> OWNER_THREAD{nullptr};

    /// Guards against a second `ktest` entering while one runs. The console
    /// dispatches one line at a time, so this only catches a future caller.
    bool RUNNING = false;

    /// The options the current run was given, published for the tests. They
    /// point into the console line, which lives for the command, and the run
    /// happens inside it, so they stay valid while a test can read them.
    Option OPTIONS[MAX_RUN_OPTIONS];
    usize OPTION_COUNT = 0;

    /// The shuffled registry order, filled only by a run that asked for a
    /// shuffle. The re-entry guard makes one run at a time the only writer,
    /// which is what lets a fixed array stand where a heap vector would.
    usize SHUFFLE_ORDER[MAX_SHUFFLE_TESTS];

    /// The SHUFFLE line's order text: the indices that will run, comma-
    /// separated. Sized for the worst plan, MAX_SHUFFLE_TESTS indices of
    /// three digits and one comma each, so the text never truncates.
    constexpr usize SHUFFLE_TEXT_SIZE = MAX_SHUFFLE_TESTS * 4 + 1;
    char SHUFFLE_TEXT[SHUFFLE_TEXT_SIZE];

    /// The SHUFFLE line's seed text: "0x" and sixteen lowercase hex digits.
    char SEED_TEXT[2 + 16 + 1];

    /// Clears the re-entry guard however the command returns.
    struct RunnerScope final {
        RunnerScope() {
            RUNNING = true;
        }
        ~RunnerScope() {
            RUNNING = false;
        }
        RunnerScope(RunnerScope const &) = delete;
        auto operator=(RunnerScope const &) -> RunnerScope & = delete;
    };

    /// The runtime hands each failure here; this is the one place a `FAIL`
    /// line is written, so the host grammar has a single producer.
    auto report(hsien::Failure const &failure) -> void {
        log::error("ktest: FAIL {} {}:{}: {}", failure.test_name,
                   std::string_view{failure.location.file_name()}, failure.location.line(),
                   std::string_view{failure.message});
    }

    /// Reports a name that appears twice. The two records would sit in
    /// different test objects and both link, so the ambiguity is caught here
    /// rather than by running an unpredictable one of them.
    auto reject_duplicates(hsien::TestRecord const *const begin, hsien::TestRecord const *const end)
        -> bool {
        for (auto const *record = begin; record != end; ++record) {
            for (auto const *other = record + 1; other != end; ++other) {
                if (std::string_view{record->name} == std::string_view{other->name}) {
                    log::error("ktest: ERROR the test name \"{}\" is registered twice",
                               record->name);
                    return true;
                }
            }
        }
        return false;
    }

    /// Runs one hook — a suite's or an environment's SetUp or TearDown — the
    /// way a test runs, minus the test: a published state and a context the
    /// hook's assertions record into, no RUN or OK line, and nothing added to
    /// the summary. True when nothing in the hook failed.
    template <typename Step>
    auto run_hook(char const *const name, Step &&step, hsien::Reporter &reporter) -> bool {
        internal::RunState state;
        CURRENT_STATE.store(&state, std::memory_order_relaxed);
        OWNER_THREAD.store(::ours::task::Thread::Current::get(), std::memory_order_relaxed);
        auto context = ::hsien::Context{name, reporter};
        internal::BindScope const scope(context);
        step(context);
        CURRENT_STATE.store(nullptr, std::memory_order_relaxed);
        OWNER_THREAD.store(nullptr, std::memory_order_relaxed);
        return context.failure_count() == 0;
    }

    /// One selected test: publish the state, run it through the runtime,
    /// then print the verdict and add it to the summary. |pre_failed| reports
    /// a test the run decided not to start — a suite or environment setup
    /// failed ahead of it — with the reason in the place the check counts go.
    auto run_test(hsien::TestRecord const &record, hsien::Reporter &reporter, Summary &summary,
                  char const *const pre_failed = nullptr) -> void {
        log::info("ktest: RUN {}", record.name);
        if (pre_failed != nullptr) {
            log::error("ktest: FAILED {} ({})", record.name, pre_failed);
            summary.record(true, false);
            return;
        }

        internal::RunState state;
        CURRENT_STATE.store(&state, std::memory_order_relaxed);
        OWNER_THREAD.store(::ours::task::Thread::Current::get(), std::memory_order_relaxed);
        auto const started = ::ours::time::SteadyClock::now();
        auto const recorded = hsien::run(&record, &record + 1, reporter);
        auto const elapsed = ::ours::time::SteadyClock::now() - started;
        CURRENT_STATE.store(nullptr, std::memory_order_relaxed);
        OWNER_THREAD.store(nullptr, std::memory_order_relaxed);

        auto const failed = recorded.failed != 0;
        if (failed) {
            log::error("ktest: FAILED {} ({} of {} checks failed)", record.name, recorded.failed,
                       state.checks);
        } else if (state.skipped) {
            log::info("ktest: SKIP {}: {}", record.name, state.skip_reason);
        } else {
            log::info("ktest: OK {} ({} us)", record.name,
                      ::ours::duration_cast<::ours::Microseconds>(elapsed).count());
        }
        summary.record(failed, state.skipped);
    }

    /// Prints the registry, one line per test, in link order.
    auto list_tests(hsien::TestRecord const *const begin, hsien::TestRecord const *const end)
        -> void {
        for (auto const *record = begin; record != end; ++record) {
            log::info("ktest: TEST {}", record->name);
        }
    }

    /// Prints the death registry after the live one, one `DEATH` line per
    /// test. The heading is the host's handle on the list: `ktest all` never
    /// runs these, so the host reads the names here and gives each one a
    /// fresh boot of its own.
    auto list_death_tests(hsien::TestRecord const *const begin, hsien::TestRecord const *const end)
        -> void {
        for (auto const *record = begin; record != end; ++record) {
            log::info("ktest: DEATH {}", record->name);
        }
    }

    /// What one environment step needs: the reporter its assertions report
    /// through, and which half of the stage is running.
    struct EnvironmentFraming {
        hsien::Reporter *reporter;
        bool setup;
    };

    /// The step the environment walk calls: runs the environment's SetUp or
    /// TearDown framed like a hook, and logs the one error line a failure
    /// needs. True when nothing in the step failed.
    auto run_environment_step(Environment &environment, void *const context) -> bool {
        auto &framing = *static_cast<EnvironmentFraming *>(context);
        auto const *name = "ktest.EnvironmentTearDown";
        if (framing.setup) {
            name = "ktest.EnvironmentSetUp";
        }
        auto const ok = run_hook(
            name,
            [&](::hsien::Context &) {
                if (framing.setup) {
                    environment.SetUp();
                } else {
                    environment.TearDown();
                }
            },
            *framing.reporter);
        if (!framing.setup && !ok) {
            log::error("ktest: ERROR the environment teardown failed");
        }
        return ok;
    }

    /// Appends |value| in decimal at |length|. The guard keeps a wrong caller
    /// inside the buffer; the plan's sizing means it never fires.
    auto append_decimal(char *const buffer, usize const capacity, usize &length, usize const value)
        -> void {
        char digits[20];
        auto count = usize{0};
        auto rest = value;
        do {
            digits[count] = static_cast<char>('0' + rest % 10);
            rest /= 10;
            count += 1;
        } while (rest != 0);
        for (auto i = usize{0}; i < count; ++i) {
            if (length + 1 >= capacity) {
                return;
            }
            buffer[length] = digits[count - 1 - i];
            length += 1;
        }
    }

    /// Writes |seed| as "0x" and sixteen lowercase hex digits, fixed width so
    /// two runs' SHUFFLE lines compare byte for byte.
    auto write_seed_text(char *const buffer, u64 const seed) -> void {
        static constexpr char DIGITS[] = "0123456789abcdef";
        buffer[0] = '0';
        buffer[1] = 'x';
        for (auto i = usize{0}; i < 16; ++i) {
            auto const shift = (15 - i) * 4;
            buffer[2 + i] = DIGITS[static_cast<usize>((seed >> shift) & 0xFu)];
        }
        buffer[18] = '\0';
    }

    /// Logs the run's shuffle, once, before the first pass: the seed it drew
    /// from and the registry indices that will run, in run order -- the
    /// shard's tests only, when one is set. Indices are the section
    /// positions `ktest list` prints, so the line names the same tests for as
    /// long as the image is unchanged.
    auto log_shuffle(usize const count, RunPlan const &plan) -> void {
        auto length = usize{0};
        for (auto pos = usize{0}; pos < count; ++pos) {
            auto const index = SHUFFLE_ORDER[pos];
            if (!in_shard(index, plan.shard_index, plan.shard_count)) {
                continue;
            }
            if (length != 0) {
                SHUFFLE_TEXT[length] = ',';
                length += 1;
            }
            append_decimal(SHUFFLE_TEXT, SHUFFLE_TEXT_SIZE, length, index);
        }
        SHUFFLE_TEXT[length] = '\0';
        write_seed_text(SEED_TEXT, plan.seed);
        log::info("ktest: SHUFFLE seed={} order={}", std::string_view{SEED_TEXT},
                  std::string_view{SHUFFLE_TEXT});
    }

    /// Runs the death tests |filter| selects. The first one that does its job
    /// panics the machine, so the walk below never reaches a second test and
    /// a full sweep is the host's job: one fresh boot per test, each given
    /// `ktest death <name>`. None of the live run's machinery applies — no
    /// environments, suite hooks or plan: a body that ends in a panic leaves
    /// nothing behind to tear down, and the machine does not come back.
    ///
    /// A body that *returns* is the failure here, the reverse of a live
    /// test's: the runner reports it FAILED, walks on to the next match and
    /// ends at a SUMMARY the host reads as "did not die".
    auto run_death_tests(std::string_view const filter) -> int {
        auto const *const begin = ktest_death_start;
        auto const *const end = ktest_death_end;
        if (begin == end) {
            log::error("ktest: ERROR no death tests are linked into this kernel");
            return 1;
        }
        if (reject_duplicates(begin, end)) {
            return 1;
        }
        auto reporter = hsien::Reporter{&report};
        auto summary = Summary{};
        for (auto const *record = begin; record != end; ++record) {
            if (!matches_filter(record->name, filter)) {
                continue;
            }
            // The line the host matches before it looks for the panic: it
            // says the body was reached, so a report after it is the body's
            // death and not some unrelated one.
            log::info("ktest: DEATH {}", record->name);
            internal::RunState state;
            CURRENT_STATE.store(&state, std::memory_order_relaxed);
            OWNER_THREAD.store(::ours::task::Thread::Current::get(), std::memory_order_relaxed);
            hsien::run(record, record + 1, reporter);
            CURRENT_STATE.store(nullptr, std::memory_order_relaxed);
            OWNER_THREAD.store(nullptr, std::memory_order_relaxed);
            log::error("ktest: FAILED {} (a death test must panic; it returned)", record->name);
            summary.record(true, false);
        }
        if (summary.ran == 0) {
            log::error("ktest: ERROR no death test matches \"{}\"", filter);
            return 1;
        }
        log::info("ktest: SUMMARY ran={} passed={} failed={} skipped={}", summary.ran,
                  summary.passed, summary.failed, summary.skipped);
        // This line is only reachable when a body returned: a death run that
        // reached its verdict is a failed one.
        return 1;
    }

    auto ktest_command(int const argc, char **const argv) -> int {
        if (RUNNING) {
            log::error("ktest: ERROR a test run is already in progress");
            return 1;
        }
        RunnerScope const scope;
        OPTION_COUNT = 0;

        // The death selection is its own command shape: one optional filter,
        // no options, none of the live run's plan. It reads its own section,
        // so a kernel without live tests still runs its death tests.
        if (argc >= 2 && std::string_view{argv[1]} == "death") {
            if (argc > 3) {
                log::error("ktest: ERROR usage: ktest death [filter]");
                return 1;
            }
            return run_death_tests(argc > 2 ? std::string_view{argv[2]} : std::string_view{});
        }

        auto const *const begin = hsien::section_begin();
        auto const *const end = hsien::section_end();
        if (begin == end) {
            log::error("ktest: ERROR no tests are linked into this kernel");
            return 1;
        }
        if (reject_duplicates(begin, end)) {
            return 1;
        }

        auto list_only = false;
        auto const *filter = "";
        // The runner's own options are only valid with `all`, which a bare
        // `ktest` also is.
        auto const all_selected = argc < 2 || std::string_view{argv[1]} == "all";
        if (argc >= 2) {
            auto const *const argument = argv[1];
            if (std::string_view{argument} == "list" || std::string_view{argument} == "?") {
                list_only = true;
            } else if (std::string_view{argument} != "all") {
                filter = argument;
            }
        }
        if (list_only && argc > 2) {
            log::error("ktest: ERROR ktest list takes no options");
            return 1;
        }
        for (auto i = 2; i < argc; ++i) {
            if (OPTION_COUNT == MAX_RUN_OPTIONS) {
                log::error("ktest: ERROR at most {} options, got {}", MAX_RUN_OPTIONS, argc - 2);
                return 1;
            }
            auto parsed = Option{};
            if (!parse_option(argv[i], parsed)) {
                log::error(
                    "ktest: ERROR usage: ktest [all|list|?|death|<filter>] [name=value ...]");
                return 1;
            }
            OPTIONS[OPTION_COUNT] = parsed;
            OPTION_COUNT += 1;
        }
        if (list_only) {
            if (reject_duplicates(ktest_death_start, ktest_death_end)) {
                return 1;
            }
            list_tests(begin, end);
            list_death_tests(ktest_death_start, ktest_death_end);
            return 0;
        }
        auto plan = RunPlan{};
        for (auto i = usize{0}; i < OPTION_COUNT; ++i) {
            auto const kind = runner_option(OPTIONS[i].name);
            if (kind != RunnerOption::Other && !all_selected) {
                log::error("ktest: ERROR shuffle, repeat and shard need \"ktest all\"");
                return 1;
            }
            switch (kind) {
                case RunnerOption::Shuffle:
                    plan.shuffle = true;
                    if (!parse_seed(OPTIONS[i].value, plan.seed)) {
                        log::error("ktest: ERROR shuffle seed is not a number: \"{}\"",
                                   OPTIONS[i].value);
                        return 1;
                    }
                    break;
                case RunnerOption::Repeat:
                    if (!parse_decimal(OPTIONS[i].value, plan.repeat) || plan.repeat == 0) {
                        log::error("ktest: ERROR repeat is not a count of at least 1: \"{}\"",
                                   OPTIONS[i].value);
                        return 1;
                    }
                    break;
                case RunnerOption::Shard:
                    if (!parse_shard(OPTIONS[i].value, plan.shard_index, plan.shard_count) ||
                        !shard_valid(plan.shard_index, plan.shard_count)) {
                        log::error("ktest: ERROR shard is not M/N with 0 <= M < N: \"{}\"",
                                   OPTIONS[i].value);
                        return 1;
                    }
                    break;
                case RunnerOption::Other:
                    break;
            }
        }
        for (auto i = usize{0}; i < OPTION_COUNT; ++i) {
            log::info("ktest: OPTION {}={}", OPTIONS[i].name, OPTIONS[i].value);
        }

        auto const count = static_cast<usize>(end - begin);
        if (plan.shuffle) {
            if (count > MAX_SHUFFLE_TESTS) {
                log::error("ktest: ERROR {} tests are linked; shuffle plans at most {}", count,
                           MAX_SHUFFLE_TESTS);
                return 1;
            }
            shuffle_indices(SHUFFLE_ORDER, count, plan.seed);
            log_shuffle(count, plan);
        }
        auto reporter = hsien::Reporter{&report};
        auto summary = Summary{};

        // The run's global stage: every registered environment's SetUp, in
        // registration order, before the first test; the TearDowns unwind
        // them after the last, however the run went. A registration dropped
        // at boot is reported here, where the run can fail on it, rather
        // than silently passing without the stage.
        auto &registry = internal::environments();
        if (registry.dropped != 0) {
            log::error("ktest: ERROR {} test environment(s) were dropped at boot: the registry "
                       "holds at most {}",
                       registry.dropped, MAX_ENVIRONMENTS);
        }
        auto environment_run = internal::EnvironmentRun{};
        auto framing = EnvironmentFraming{&reporter, true};
        auto const environments_ok = environment_run.set_up(run_environment_step, &framing);
        if (!environments_ok) {
            log::error("ktest: ERROR the environment setup failed; every test will be reported "
                       "failed without running");
        }
        framing.setup = false;
        auto tear_environments = [&]() -> void {
            environment_run.tear_down(run_environment_step, &framing);
        };

        // The suite hooks frame one pass: a suite's setup runs once before
        // its first test of the pass and its teardown once after the last, so
        // a repeat hands every suite a fresh stage each pass. A run that
        // failed an environment setup reports its tests without running
        // either them or the suite hooks around them.
        for (auto pass = usize{0}; pass < plan.repeat; ++pass) {
            auto suites =
                internal::SuitePlan{ktest_suite_hooks_start, ktest_suite_hooks_end, begin, end,
                                    [&](hsien::TestRecord const &record, usize const index) {
                                        return matches_filter(record.name, filter) &&
                                               in_shard(index, plan.shard_index, plan.shard_count);
                                    }};
            if (!suites.valid()) {
                log::error("ktest: ERROR a suite declares a hook twice, or more suites carry "
                           "hooks than a run tracks");
                tear_environments();
                return 1;
            }
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
                if (!environments_ok) {
                    run_test(record, reporter, summary, "the environment setup failed");
                    continue;
                }
                if (auto *const setup = suites.before(record.name); setup != nullptr) {
                    if (!run_hook(setup->name, setup->invoke, reporter)) {
                        suites.poison(record.name);
                        log::error("ktest: ERROR the suite setup {} failed; its tests will not "
                                   "run",
                                   setup->name);
                    }
                }
                if (suites.poisoned(record.name)) {
                    run_test(record, reporter, summary, "its suite setup failed");
                } else {
                    run_test(record, reporter, summary);
                }
                if (auto *const teardown = suites.after(record.name); teardown != nullptr) {
                    if (!run_hook(teardown->name, teardown->invoke, reporter)) {
                        log::error("ktest: ERROR the suite teardown {} failed", teardown->name);
                    }
                }
            }
        }
        tear_environments();
        if (summary.ran == 0) {
            if (plan.shard_count > 1) {
                log::error("ktest: ERROR no test falls in shard {}/{} among {} tests",
                           plan.shard_index, plan.shard_count, count);
            } else {
                log::error("ktest: ERROR no test matches \"{}\"", filter);
            }
        }
        log::info("ktest: SUMMARY ran={} passed={} failed={} skipped={}", summary.ran,
                  summary.passed, summary.failed, summary.skipped);
        if (summary.failed != 0 || summary.ran == 0) {
            return 1;
        }
        return 0;
    }

} // namespace

namespace internal {

    auto current_state() -> RunState * {
        return CURRENT_STATE.load(std::memory_order_relaxed);
    }

    auto on_test_thread() -> bool {
        auto *const owner = OWNER_THREAD.load(std::memory_order_relaxed);
        return owner != nullptr && ::ours::task::Thread::Current::get() == owner;
    }

    auto framework_error(char const *const what) -> void {
        log::error("ktest: ERROR {}", what);
    }

    auto bind_context(::hsien::Context &context) -> void {
        auto *const state = CURRENT_STATE.load(std::memory_order_relaxed);
        if (state == nullptr) {
            framework_error("a test body ran with no test state published");
            return;
        }
        state->context = &context;
    }

    auto unbind_context() -> void {
        auto *const state = CURRENT_STATE.load(std::memory_order_relaxed);
        if (state != nullptr) {
            state->context = nullptr;
        }
    }

    auto record_skip(char const *const reason) -> void {
        auto *const state = CURRENT_STATE.load(std::memory_order_relaxed);
        if (state == nullptr) {
            framework_error("KTEST_SKIP ran with no test running");
            return;
        }
        if (!on_test_thread()) {
            framework_error("KTEST_SKIP ran on a thread that is not running the test");
            return;
        }
        // The runner prints the reason after the body has returned, so it
        // cannot keep the caller's pointer: copy it into the state, truncated
        // to fit.
        auto const text = std::string_view{reason};
        auto const room = sizeof(state->skip_reason) - 1;
        auto const count = text.size() < room ? text.size() : room;
        for (auto i = usize{0}; i < count; ++i) {
            state->skip_reason[i] = text[i];
        }
        state->skip_reason[count] = '\0';
        state->skipped = true;
    }

} // namespace internal

auto has_option(std::string_view const name) -> bool {
    return find_option(OPTIONS, OPTION_COUNT, name) != nullptr;
}

auto option(std::string_view const name, std::string_view const fallback) -> std::string_view {
    auto const *const found = find_option(OPTIONS, OPTION_COUNT, name);
    return found == nullptr ? fallback : found->value;
}

auto option_usize(std::string_view const name, usize const fallback) -> usize {
    auto const *const found = find_option(OPTIONS, OPTION_COUNT, name);
    if (found == nullptr) {
        return fallback;
    }
    if (found->value.empty()) {
        log::error("ktest: ERROR option \"{}\" needs a number", name);
        return fallback;
    }
    auto value = usize{0};
    for (auto const c: found->value) {
        if (c < '0' || c > '9') {
            log::error("ktest: ERROR option \"{}\" is not a number: \"{}\"", name, found->value);
            return fallback;
        }
        value = value * 10 + static_cast<usize>(c - '0');
    }
    return value;
}

} // namespace ktest

GKTL_COMMAND(Ktest, "ktest", ktest::ktest_command,
             "run the kernel tests: ktest [all|list|?|death|<filter>]");
