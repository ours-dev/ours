/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The death-test smoke: one `KTEST_DEATH` whose body panics on demand. Its
// record lands in `.ktest.death`, which the live registry never walks, so
// `ktest all` stays exactly as wide as before and `ktest list` gains one
// `DEATH` line; the host's `ktest death` loop gives it a fresh boot, types
// its name and judges the panic report the body ends in.
//
// The message is fixed so a human reading the serial log knows which report
// is the test's, and so the panic the host judged is recognizably this body's
// and not some unrelated one.
#include <ours/assert.hpp>

#include <ktest/ktest.hpp>

namespace {

KTEST_DEATH(KtestDeath, PanicsOnDemand) {
    ours::panic("ktest: death test reached its panic");
}

} // namespace
