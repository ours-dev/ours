/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The fixture feature's booted smoke: KTEST_F tests whose SetUp writes a
// member the body reads, a suite-setup counter that proves once per suite, a
// fresh-fixture pair that proves the object is rebuilt per test, and an
// environment registered at boot that proves it came up before the tests.
// Deliberately small: this is the frame's own proof, not a model for
// subsystem tests.
#include <ktest/ktest.hpp>

namespace {

/// The suite's bookkeeping. The suite teardown clears the counters, so
/// every `ktest` run of this suite starts from zero and proves the same
/// counts again.
constexpr usize WRITTEN_IN_SETUP = 42;

usize SUITE_SETUPS = 0;
usize FIXTURE_TEARDOWNS = 0;
bool ENVIRONMENT_READY = false;

// Whether each of the suite's three tests has started, this pass. A test
// that claims something about the tests before it -- that no teardown has
// run yet, that both earlier teardowns have -- derives the claim from these
// instead of from the section order: a shuffled run runs the suite in
// another order, and the claims hold there too, derived from who actually
// ran ahead. Tests run one at a time, so at a body's first line every test
// started before it has also finished, its teardown included.
bool READS_RAN = false;
bool MUTATES_RAN = false;
bool FRESH_RAN = false;

class KtestFixtureEnvironment: public ktest::Environment {
  public:
    void SetUp() override {
        ENVIRONMENT_READY = true;
    }

    void TearDown() override {
        ENVIRONMENT_READY = false;
    }
};

KtestFixtureEnvironment ENVIRONMENT;

// Registered with the kernel's other boot-time initializers: one pointer
// into the fixed registry. A dropped registration is what the run
// reports, and what the first test below asserts on.
auto const REGISTERED = ktest::add_environment(ENVIRONMENT);

class KtestFixture: public ktest::Fixture<KtestFixture> {
  public:
    usize written_in_setup{};

    void SetUp() {
        written_in_setup = WRITTEN_IN_SETUP;
    }

    void TearDown() {
        FIXTURE_TEARDOWNS += 1;
    }
};

KTEST_SUITE_SETUP(KtestFixture) {
    SUITE_SETUPS += 1;
}

KTEST_SUITE_TEARDOWN(KtestFixture) {
    SUITE_SETUPS = 0;
    FIXTURE_TEARDOWNS = 0;
    READS_RAN = false;
    MUTATES_RAN = false;
    FRESH_RAN = false;
}

KTEST_F(KtestFixture, ReadsWhatSetUpWrote) {
    READS_RAN = true;
    EXPECT_EQ(this->written_in_setup, WRITTEN_IN_SETUP);
    EXPECT_EQ(SUITE_SETUPS, usize{1}, "the suite setup ran once, before this test");
    EXPECT_TRUE(REGISTERED, "the environment registered at boot");
    EXPECT_TRUE(ENVIRONMENT_READY, "the environment came up before the tests");
    EXPECT_EQ(FIXTURE_TEARDOWNS, usize{MUTATES_RAN} + usize{FRESH_RAN},
              "every suite mate that ran before this one has had its teardown");
    // The failure path, by hand: `ktest KtestFixture.* fail=1` asks one
    // check to fail, so a manual run can see TearDown run after a failure
    // and the suite teardown still come after it. The default run stays
    // green.
    if (ktest::has_option("fail")) {
        ADD_FAILURE("the run options asked for a failure");
    }
}

KTEST_F(KtestFixture, MutatesThenHandsOver) {
    MUTATES_RAN = true;
    EXPECT_EQ(this->written_in_setup, WRITTEN_IN_SETUP);
    this->written_in_setup = 7;
}

KTEST_F(KtestFixture, GetsAFreshSetUp) {
    FRESH_RAN = true;
    EXPECT_EQ(this->written_in_setup, WRITTEN_IN_SETUP,
              "a new fixture per test: the mutation the earlier test made is gone");
    EXPECT_EQ(SUITE_SETUPS, usize{1});
    EXPECT_EQ(FIXTURE_TEARDOWNS, usize{READS_RAN} + usize{MUTATES_RAN},
              "both suite mates that ran before this one have had their teardowns");
}

} // namespace
