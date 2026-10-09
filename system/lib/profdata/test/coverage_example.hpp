/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROFDATA_TEST_COVERAGE_EXAMPLE_HPP
#define PROFDATA_TEST_COVERAGE_EXAMPLE_HPP 1

namespace coverage_example {

/// A function with branches, for the tests to call and so move counters. It is
/// built with coverage instrumentation whatever variant the test is built in,
/// so that the tests always have something to dump.
auto classify(int value) -> int;

/// Never called by any test: its counters stay at zero.
auto never_called() -> int;

} // namespace coverage_example

#endif // #ifndef PROFDATA_TEST_COVERAGE_EXAMPLE_HPP
