/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The `ktest` console command: walks the runtime's record section, applies a
// selection and a run plan (shuffle, repeat, shard), and runs each selected
// test through the runtime one at a time so it can frame and time it. Its
// lines are the host's contract: `RUN`, `OK`, `FAIL`, `FAILED`, `SKIP`,
// `TEST`, `ERROR`, the `SHUFFLE` line a shuffled run opens with, and the
// final `SUMMARY`.
#include <ktest/filter.hpp>
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

    /// One selected test: publish the state, run it through the runtime,
    /// then print the verdict and add it to the summary.
    auto run_test(hsien::TestRecord const &record, hsien::Reporter &reporter, Summary &summary)
        -> void {
        log::info("ktest: RUN {}", record.name);

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

    auto ktest_command(int const argc, char **const argv) -> int {
        if (RUNNING) {
            log::error("ktest: ERROR a test run is already in progress");
            return 1;
        }
        RunnerScope const scope;
        OPTION_COUNT = 0;

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
                log::error("ktest: ERROR usage: ktest [all|list|?|<filter>] [name=value ...]");
                return 1;
            }
            OPTIONS[OPTION_COUNT] = parsed;
            OPTION_COUNT += 1;
        }
        if (list_only) {
            list_tests(begin, end);
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
             "run the kernel tests: ktest [all|list|?|<filter>]");
