/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The stand-in runner's definitions: the shared state the host suite's cases
// reset between themselves, the reporter that captures failures, and the
// ktest::internal seam functions the booted runner defines in runner.cpp —
// the wrong-thread guard, whose answer is fixed here because the host runs
// one thread, and the framework-error channel, which counts instead of
// logging. The published state itself is the shared stage runner's.
#include "host_shim.hpp"

#include <ktest/run_stage.hpp>

#include <string_view>

#include <cstdlib>

// The assertion the gktl primitives carry (parse_u64's base check) ends in
// ours::vpanic, which the kernel defines and the host suite stands in with
// an abort -- the misuse paths it guards never run in a passing suite.
namespace ours {
auto vpanic(std::string_view) -> void {
    std::abort();
}
} // namespace ours

namespace {

auto note_failure(hsien::Failure const &failure) -> void {
    FAILURES.push_back(std::string{failure.test_name} + ": " + failure.message);
}

} // namespace

std::vector<std::string> FAILURES;
usize FRAMEWORK_ERRORS = 0;
std::vector<std::string> LOG;
hsien::Reporter REPORTER{&note_failure};

namespace ktest::internal {

auto current_state() -> RunState * {
    return CURRENT_STATE.load(std::memory_order_relaxed);
}

auto on_test_thread() -> bool {
    return true;
}

auto framework_error(char const *what) -> void {
    FRAMEWORK_ERRORS += 1;
    FAILURES.push_back(std::string{"framework: "} + what);
}

auto bind_stage_thread() -> void {}

auto unbind_stage_thread() -> void {}

auto bind_context(::hsien::Context &context) -> void {
    auto *const state = CURRENT_STATE.load(std::memory_order_relaxed);
    if (state != nullptr) {
        state->context = &context;
    }
}

auto unbind_context() -> void {
    auto *const state = CURRENT_STATE.load(std::memory_order_relaxed);
    if (state != nullptr) {
        state->context = nullptr;
    }
}

auto record_skip(char const *reason) -> void {
    auto *const state = CURRENT_STATE.load(std::memory_order_relaxed);
    if (state == nullptr) {
        framework_error("KTEST_SKIP ran with no test running");
        return;
    }
    // The runner prints the reason after the body has returned, so it cannot
    // keep the caller's pointer: copy it into the state, truncated to fit.
    auto const text = std::string_view{reason};
    auto const room = sizeof(state->skip_reason) - 1;
    auto count = text.size();
    if (count > room) {
        count = room;
    }
    for (auto i = usize{0}; i < count; ++i) {
        state->skip_reason[i] = text[i];
    }
    state->skip_reason[count] = '\0';
    state->skipped = true;
}

} // namespace ktest::internal

auto run_record(hsien::TestRecord const &record) -> RunOutcome {
    ktest::internal::RunState state;
    ktest::internal::CURRENT_STATE.store(&state, std::memory_order_relaxed);
    hsien::run(&record, &record + 1, REPORTER);
    ktest::internal::CURRENT_STATE.store(nullptr, std::memory_order_relaxed);
    auto outcome = RunOutcome{};
    outcome.failures = state.checks;
    outcome.skipped = state.skipped;
    return outcome;
}
