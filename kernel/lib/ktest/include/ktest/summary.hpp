/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// What one `ktest` run adds up to. Kept free of kernel headers so the host
// tests can exercise the counting the SUMMARY line reports.
#ifndef KTEST_SUMMARY_HPP
#define KTEST_SUMMARY_HPP 1

namespace ktest {

struct Summary {
    usize ran{};
    usize passed{};
    usize failed{};
    usize skipped{};

    /// Records one finished test. A test that failed is counted as failed
    /// even if it also asked to be skipped; a skip is not a failure.
    auto record(bool const test_failed, bool const test_skipped) -> void {
        ++ran;
        if (test_failed) {
            ++failed;
            return;
        }
        if (test_skipped) {
            ++skipped;
            return;
        }
        ++passed;
    }
};

} // namespace ktest

#endif // #ifndef KTEST_SUMMARY_HPP
