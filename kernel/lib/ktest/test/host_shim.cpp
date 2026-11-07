/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The stand-in runner's definitions: the shared state the host suite's cases
// reset between themselves, the reporter that captures failures, and the six
// ktest::internal functions runner.cpp defines on the kernel side.
#include "host_shim.hpp"

namespace {

auto note_failure(hsien::Failure const &failure) -> void {
    FAILURES.push_back(std::string{failure.test_name} + ": " + failure.message);
}

} // namespace

ktest::internal::RunState *HOST_PUBLISHED = nullptr;
std::vector<std::string> FAILURES;
usize FRAMEWORK_ERRORS = 0;
std::vector<std::string> LOG;
hsien::Reporter REPORTER{&note_failure};

namespace ktest::internal {

auto current_state() -> RunState * {
    return HOST_PUBLISHED;
}

auto on_test_thread() -> bool {
    return true;
}

auto framework_error(char const *what) -> void {
    FRAMEWORK_ERRORS += 1;
    FAILURES.push_back(std::string{"framework: "} + what);
}

auto bind_context(::hsien::Context &context) -> void {
    if (HOST_PUBLISHED != nullptr) {
        HOST_PUBLISHED->context = &context;
    }
}

auto unbind_context() -> void {
    if (HOST_PUBLISHED != nullptr) {
        HOST_PUBLISHED->context = nullptr;
    }
}

auto record_skip(char const *reason) -> void {
    if (HOST_PUBLISHED == nullptr) {
        framework_error("KTEST_SKIP ran with no test running");
        return;
    }
    auto const text = std::string_view{reason};
    auto const room = sizeof(HOST_PUBLISHED->skip_reason) - 1;
    auto const count = text.size() < room ? text.size() : room;
    for (auto i = usize{0}; i < count; ++i) {
        HOST_PUBLISHED->skip_reason[i] = text[i];
    }
    HOST_PUBLISHED->skip_reason[count] = '\0';
    HOST_PUBLISHED->skipped = true;
}

} // namespace ktest::internal

auto run_record(hsien::TestRecord const &record) -> RunOutcome {
    ktest::internal::RunState state;
    HOST_PUBLISHED = &state;
    hsien::run(&record, &record + 1, REPORTER);
    HOST_PUBLISHED = nullptr;
    auto outcome = RunOutcome{};
    outcome.failures = state.checks;
    outcome.skipped = state.skipped;
    return outcome;
}
