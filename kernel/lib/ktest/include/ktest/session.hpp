/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// One test run. It owns the plan, the shuffle order, the summary and the
// state the test currently running publishes. Logging and the steady clock
// come from the Services member: the kernel supplies its logger and clock,
// and the algorithm does not name either. The object is large on purpose
// (the shuffle buffer), so the one console command keeps it in static
// storage — a test body nests on the same kernel stack as the command.
#ifndef KTEST_SESSION_HPP
#define KTEST_SESSION_HPP 1

#include <ktest/environment.hpp>
#include <ktest/filter.hpp>
#include <ktest/fixture.hpp>
#include <ktest/run_plan.hpp>
#include <ktest/run_stage.hpp>
#include <ktest/summary.hpp>

#include <atomic>
#include <span>
#include <string_view>
#include <utility>

#include <hsien/runner.hpp>

namespace ktest {

/// |Services| provides `info`, `error`, `now` and `microseconds_since`.
/// The thread check stays an ordinary function: assertion macros expand in
/// every test translation unit, and a template parameter there would pull
/// the kernel thread type into those files.
template <typename Services>
class Run {
  public:
    explicit Run(Services services) : m_services(std::move(services)) {}

    /// The run tests read options from. Valid while this run is entered.
    [[nodiscard]]
    auto plan() const -> RunPlan const & {
        return m_plan;
    }

    auto adopt(RunPlan plan) -> void {
        m_plan = plan;
    }

    /// True when some name is registered twice. The run stops before it
    /// would otherwise pick one of the two.
    [[nodiscard]]
    auto names_repeat(hsien::TestRegistry const tests) -> bool {
        return reject_duplicates(tests);
    }

    /// One run at a time. False when a run is already entered.
    [[nodiscard]]
    auto enter() -> bool {
        if (m_running) {
            return false;
        }
        m_running = true;
        return true;
    }

    auto leave() -> void {
        m_running = false;
    }

    [[nodiscard]]
    auto list(hsien::TestRegistry const tests, hsien::TestRegistry const deaths) -> int {
        if (tests.empty()) {
            m_services.error("ktest: ERROR no tests are linked into this kernel");
            return 1;
        }
        if (reject_duplicates(tests) || reject_duplicates(deaths)) {
            return 1;
        }
        for (auto const &record: tests.records()) {
            m_services.info("ktest: TEST {}", record.name);
        }
        for (auto const &record: deaths.records()) {
            m_services.info("ktest: DEATH {}", record.name);
        }
        return 0;
    }

    /// The live selection. |m_plan| is the plan adopt() installed.
    [[nodiscard]]
    auto execute(hsien::TestRegistry const tests, internal::SuiteHook const *const hooks_begin,
                 internal::SuiteHook const *const hooks_end, std::string_view const filter) -> int {
        if (reject_duplicates(tests)) {
            return 1;
        }
        auto const records = tests.records();
        auto const *const begin = records.data();
        auto const *const end = begin + records.size();
        m_summary = Summary{};
        for (auto i = usize{0}; i < m_plan.option_count; ++i) {
            m_services.info("ktest: OPTION {}={}", m_plan.options[i].name, m_plan.options[i].value);
        }

        auto const count = records.size();
        if (m_plan.shuffle) {
            if (count > MAX_SHUFFLE_TESTS) {
                m_services.error("ktest: ERROR {} tests are linked; shuffle plans at most {}",
                                 count, MAX_SHUFFLE_TESTS);
                return 1;
            }
            shuffle(std::span{m_order, count}, m_plan.seed);
            log_shuffle(count);
        }

        auto &reporter = m_reporter;
        auto &registry = internal::environments();
        if (registry.dropped != 0) {
            m_services.error(
                "ktest: ERROR {} test environment(s) were dropped at boot: the registry "
                "holds at most {}",
                registry.dropped, MAX_ENVIRONMENTS);
        }
        auto environment_run = internal::EnvironmentRun{};
        auto const environments_ok = environment_run.set_up(reporter);
        if (!environments_ok) {
            m_services.error(
                "ktest: ERROR the environment setup failed; every test will be reported "
                "failed without running");
        }
        auto tear_environments = [&]() -> void { environment_run.tear_down(reporter); };

        for (auto pass = usize{0}; pass < m_plan.repeat; ++pass) {
            auto suites =
                internal::SuitePlan{hooks_begin, hooks_end, begin, end,
                                    [&](hsien::TestRecord const &record, usize const index) {
                                        return matches_filter(record.name, filter) &&
                                               in_shard(index, m_plan.shard.m, m_plan.shard.n);
                                    }};
            if (!suites.valid()) {
                m_services.error("ktest: ERROR a suite declares a hook twice, or more suites carry "
                                 "hooks than a run tracks");
                tear_environments();
                return 1;
            }
            for (auto pos = usize{0}; pos < count; ++pos) {
                auto index = pos;
                if (m_plan.shuffle) {
                    index = m_order[pos];
                }
                auto const &record = records[index];
                if (!matches_filter(record.name, filter) ||
                    !in_shard(index, m_plan.shard.m, m_plan.shard.n)) {
                    continue;
                }
                if (!environments_ok) {
                    run_test(record, reporter, "the environment setup failed");
                    continue;
                }
                if (auto *const setup = suites.before(record.name); setup != nullptr) {
                    if (!internal::run_hook(setup->name, setup->invoke, reporter)) {
                        suites.poison(record.name);
                        m_services.error("ktest: ERROR the suite setup {} failed; its tests will "
                                         "not run",
                                         setup->name);
                    }
                }
                if (suites.poisoned(record.name)) {
                    run_test(record, reporter, "its suite setup failed");
                } else {
                    run_test(record, reporter, nullptr);
                }
                if (auto *const teardown = suites.after(record.name); teardown != nullptr) {
                    if (!internal::run_hook(teardown->name, teardown->invoke, reporter)) {
                        m_services.error("ktest: ERROR the suite teardown {} failed",
                                         teardown->name);
                    }
                }
            }
        }
        tear_environments();
        if (m_summary.ran == 0) {
            if (m_plan.shard.n > 1) {
                m_services.error("ktest: ERROR no test falls in shard {}/{} among {} tests",
                                 m_plan.shard.m, m_plan.shard.n, count);
            } else {
                m_services.error("ktest: ERROR no test matches \"{}\"", filter);
            }
        }
        m_services.info("ktest: SUMMARY ran={} passed={} failed={} skipped={}", m_summary.ran,
                        m_summary.passed, m_summary.failed, m_summary.skipped);
        if (m_summary.failed != 0 || m_summary.ran == 0) {
            return 1;
        }
        return 0;
    }

    /// Death tests. A body that returns is the failure; the first panic ends
    /// the machine, so a second match is the host's next boot.
    [[nodiscard]]
    auto execute_death(hsien::TestRegistry const deaths, std::string_view const filter) -> int {
        if (deaths.empty()) {
            m_services.error("ktest: ERROR no death tests are linked into this kernel");
            return 1;
        }
        if (reject_duplicates(deaths)) {
            return 1;
        }
        m_summary = Summary{};
        for (auto const &record: deaths.records()) {
            if (!matches_filter(record.name, filter)) {
                continue;
            }
            m_services.info("ktest: DEATH {}", record.name);
            publish();
            static_cast<void>(record.run(m_reporter));
            unpublish();
            m_services.error("ktest: FAILED {} (a death test must panic; it returned)",
                             record.name);
            m_summary.record(true, false);
        }
        if (m_summary.ran == 0) {
            m_services.error("ktest: ERROR no death test matches \"{}\"", filter);
            return 1;
        }
        m_services.info("ktest: SUMMARY ran={} passed={} failed={} skipped={}", m_summary.ran,
                        m_summary.passed, m_summary.failed, m_summary.skipped);
        return 1;
    }

  private:
    /// Prints every failed check as the console's `FAIL` line. The run owns
    /// one and hands it to every test, hook and stage; holding the services
    /// is what lets a failure reach the run's logger without a global slot
    /// pointing back at the run.
    class FailureLines final: public hsien::Reporter {
      public:
        explicit FailureLines(Services const &services) : m_services(&services) {}

        auto report(hsien::Failure const &failure) -> void override {
            m_services->error("ktest: FAIL {} {}:{}: {}", failure.test_name,
                              std::string_view{failure.location.file_name()},
                              failure.location.line(), std::string_view{failure.message});
        }

      private:
        Services const *m_services;
    };

    auto reject_duplicates(hsien::TestRegistry const tests) -> bool {
        auto const records = tests.records();
        for (auto i = usize{0}; i < records.size(); ++i) {
            for (auto j = i + 1; j < records.size(); ++j) {
                if (std::string_view{records[i].name} == std::string_view{records[j].name}) {
                    m_services.error("ktest: ERROR the test name \"{}\" is registered twice",
                                     records[i].name);
                    return true;
                }
            }
        }
        return false;
    }

    auto publish() -> void {
        m_state = internal::RunState{};
        internal::CURRENT_STATE.store(&m_state, std::memory_order_relaxed);
        internal::bind_stage_thread();
    }

    auto unpublish() -> void {
        internal::CURRENT_STATE.store(nullptr, std::memory_order_relaxed);
        internal::unbind_stage_thread();
    }

    auto run_test(hsien::TestRecord const &record, hsien::Reporter &reporter,
                  char const *const pre_failed) -> void {
        m_services.info("ktest: RUN {}", record.name);
        if (pre_failed != nullptr) {
            m_services.error("ktest: FAILED {} ({})", record.name, pre_failed);
            m_summary.record(true, false);
            return;
        }

        publish();
        auto const started = m_services.now();
        auto const failures = record.run(reporter);
        auto const elapsed_us = m_services.microseconds_since(started);
        auto const checks = m_state.checks;
        auto const skipped = m_state.skipped;
        char reason[internal::SKIP_REASON_ROOM];
        for (auto i = usize{0}; i < sizeof(reason); ++i) {
            reason[i] = m_state.skip_reason[i];
        }
        unpublish();

        auto const failed = failures != 0;
        if (failed) {
            m_services.error("ktest: FAILED {} ({} of {} checks failed)", record.name, failures,
                             checks);
        } else if (skipped) {
            m_services.info("ktest: SKIP {}: {}", record.name, reason);
        } else {
            m_services.info("ktest: OK {} ({} us)", record.name, elapsed_us);
        }
        m_summary.record(failed, skipped);
    }

    auto append_decimal(usize &length, usize const value) -> void {
        char digits[20];
        auto count = usize{0};
        auto rest = value;
        do {
            digits[count] = static_cast<char>('0' + rest % 10);
            rest /= 10;
            count += 1;
        } while (rest != 0);
        for (auto i = usize{0}; i < count; ++i) {
            if (length + 1 >= sizeof(m_shuffle_text)) {
                return;
            }
            m_shuffle_text[length] = digits[count - 1 - i];
            length += 1;
        }
    }

    auto log_shuffle(usize const count) -> void {
        auto length = usize{0};
        for (auto pos = usize{0}; pos < count; ++pos) {
            auto const index = m_order[pos];
            if (!in_shard(index, m_plan.shard.m, m_plan.shard.n)) {
                continue;
            }
            if (length != 0) {
                m_shuffle_text[length] = ',';
                length += 1;
            }
            append_decimal(length, index);
        }
        m_shuffle_text[length] = '\0';
        static constexpr char DIGITS[] = "0123456789abcdef";
        m_seed_text[0] = '0';
        m_seed_text[1] = 'x';
        for (auto i = usize{0}; i < 16; ++i) {
            auto const shift = (15 - i) * 4;
            m_seed_text[2 + i] = DIGITS[static_cast<usize>((m_plan.seed >> shift) & 0xFu)];
        }
        m_seed_text[18] = '\0';
        m_services.info("ktest: SHUFFLE seed={} order={}", std::string_view{m_seed_text},
                        std::string_view{m_shuffle_text});
    }

    Services m_services;
    FailureLines m_reporter{m_services};
    RunPlan m_plan{};
    Summary m_summary{};
    internal::RunState m_state{};
    bool m_running{false};
    usize m_order[MAX_SHUFFLE_TESTS]{};
    char m_shuffle_text[MAX_SHUFFLE_TESTS * 4 + 1]{};
    char m_seed_text[2 + 16 + 1]{};
};

} // namespace ktest

#endif // #ifndef KTEST_SESSION_HPP
