/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The interface's own smoke tests. They are here so the image that carries the
// test interface always carries at least one real record, and so a booted
// `ktest` run exercises the declaration macro, both assertion families and the
// skip state. Subsystem tests arrive with the pilot migration; this file is
// not a model for them, it is the smallest proof that the frame works.
#include <ours/status.hpp>

#include <ktest/ktest.hpp>

namespace {

/// The checks the interface offers, all passing, so a run that reaches here
/// proves the macros compile, evaluate once and reach the runtime.
KTEST(Ktest, AssertionsPass) {
    auto const value = 6 * 7;

    EXPECT_TRUE(value == 42);
    EXPECT_FALSE(value == 41);
    EXPECT_EQ(value, 42);
    EXPECT_NE(value, 41);
    EXPECT_LT(value, 43);
    EXPECT_LE(value, 42);
    EXPECT_GT(value, 41);
    EXPECT_GE(value, 42);

    auto const *const pointer = &value;
    EXPECT_NONNULL(pointer);
    EXPECT_OK(ours::Status::Ok);

    // ASSERT returns on failure, so it is only usable where the compiler can
    // see a void function; the test body is one.
    ASSERT_EQ(*pointer, 42);
}

/// A skip is reported as skipped, not as a pass and not as a failure.
KTEST(Ktest, SkipIsReported) {
    KTEST_SKIP("the interface's skip state is what this test checks");
}

/// The name carries a `_` on purpose: test names are compared as strings and
/// never go through the console command encoder, unlike a command's name.
KTEST(Ktest, Names_MayHaveUnderscores) {
    EXPECT_TRUE(true);
}

} // namespace
