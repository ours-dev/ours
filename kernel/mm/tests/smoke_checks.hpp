/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The tally a host mm smoke test keeps: one line per check, then one verdict
/// line and the exit code main() returns. A test that uses it runs every check
/// and fails if any failed, rather than stopping at the first.
///
/// The printed lines are the interface the host test runner reads, so their
/// text is fixed: "<name>: pass  <what>", "<name>: FAIL  <what>", and at the end
/// "<name>: FAILED -- <n> check(s)" or "<name>: ok".
#ifndef OURS_MM_TESTS_SMOKE_CHECKS_HPP
#define OURS_MM_TESTS_SMOKE_CHECKS_HPP 1

#include <cstdio>

namespace ours::mm::test {
class SmokeChecks {
  public:
    /// `name` prefixes every line printed, and must outlive this object; a
    /// string literal does.
    explicit SmokeChecks(char const *const name) : m_name(name) {}

    /// Records one check and prints whether it passed. A failure is counted,
    /// not returned on, so the checks after it still run and print.
    auto check(bool const ok, char const *const what) -> void {
        if (ok) {
            std::printf("%s: pass  %s\n", m_name, what);
            return;
        }
        m_failures += 1;
        std::printf("%s: FAIL  %s\n", m_name, what);
    }

    /// Prints the final line and answers main()'s exit code: 1 if any check
    /// failed, 0 otherwise.
    [[nodiscard]]
    auto verdict() const -> int {
        if (m_failures != 0) {
            std::printf("%s: FAILED -- %d check(s)\n", m_name, m_failures);
            return 1;
        }
        std::printf("%s: ok\n", m_name);
        return 0;
    }

  private:
    char const *m_name;
    int m_failures = 0;
};

} // namespace ours::mm::test

#endif // #ifndef OURS_MM_TESTS_SMOKE_CHECKS_HPP
