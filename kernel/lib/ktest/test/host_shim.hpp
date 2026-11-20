/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The runner's kernel-only half, stood in for the host suite: the seam the
// assertion machinery calls through and the failure capture the stand-in's
// reporter fills. The suite's host tests share it so that every file can run
// real macro expansions through hsien's own record loop, which is the seam
// the booted runner implements. The published state itself is the shared
// stage runner's (<ktest/run_stage.hpp>), the same slot the booted runner
// publishes.
#ifndef KTEST_TEST_HOST_SHIM_HPP
#define KTEST_TEST_HOST_SHIM_HPP 1

#include <ktest/ktest.hpp>

#include <string>
#include <vector>

#include <hsien/runner.hpp>

/// Every failure the run's assertions record, as the booted runner would
/// print them; the cases assert on the count and sometimes the text.
extern std::vector<std::string> FAILURES;

/// Framework errors — an assertion with no test running and the like —
/// which a well-behaved case never causes.
extern usize FRAMEWORK_ERRORS;

/// What the test bodies did, in order. Every step of every fixture in the
/// suite appends here, so a case can assert the exact sequence.
extern std::vector<std::string> LOG;

/// The reporter every run_record hands to the runtime.
extern hsien::Reporter REPORTER;

/// What one record's run produced: the failures its assertions recorded
/// and whether it asked to be skipped — the inputs the booted runner
/// turns into the FAILED/SKIP/OK verdict.
struct RunOutcome {
    usize failures{};
    bool skipped{};
};

/// Runs one record the way the runner frames a test: publish the state,
/// hand the record to hsien's loop, publish nothing again.
auto run_record(hsien::TestRecord const &record) -> RunOutcome;

#endif // #ifndef KTEST_TEST_HOST_SHIM_HPP
