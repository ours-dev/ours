/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The `ktest` console command: walks the runtime's record section, applies a
// selection, and runs each selected test through the runtime one at a time so
// it can frame and time it. Its lines are the host's contract: `RUN`, `OK`,
// `FAIL`, `FAILED`, `SKIP`, `TEST`, `ERROR` and the final `SUMMARY`.
#include <ktest/filter.hpp>
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

    auto ktest_command(int const argc, char **const argv) -> int {
        if (RUNNING) {
            log::error("ktest: ERROR a test run is already in progress");
            return 1;
        }
        RunnerScope const scope;

        auto const *const begin = hsien::section_begin();
        auto const *const end = hsien::section_end();
        if (begin == end) {
            log::error("ktest: ERROR no tests are linked into this kernel");
            return 1;
        }
        if (reject_duplicates(begin, end)) {
            return 1;
        }

        if (argc > 2) {
            log::error("ktest: ERROR usage: ktest [all|list|?|<filter>]");
            return 1;
        }
        auto list_only = false;
        auto const *filter = "";
        if (argc == 2) {
            auto const *const argument = argv[1];
            if (std::string_view{argument} == "list" || std::string_view{argument} == "?") {
                list_only = true;
            } else if (std::string_view{argument} != "all") {
                filter = argument;
            }
        }
        if (list_only) {
            list_tests(begin, end);
            return 0;
        }

        auto reporter = hsien::Reporter{&report};
        auto summary = Summary{};
        for (auto const *record = begin; record != end; ++record) {
            if (!matches_filter(record->name, filter)) {
                continue;
            }
            run_test(*record, reporter, summary);
        }
        if (summary.ran == 0) {
            log::error("ktest: ERROR no test matches \"{}\"", filter);
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
} // namespace ktest

GKTL_COMMAND(Ktest, "ktest", ktest::ktest_command,
             "run the kernel tests: ktest [all|list|?|<filter>]");
