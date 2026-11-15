/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The fixture half of the interface, on the host: the lifecycle a KTEST_F
// adapter runs, the counting it feeds the runner's verdict, the suite plan's
// once-per-suite decisions and the environment walk, all against the real
// macro expansions driven through hsien's own record loop.
//
// The runner's kernel-only half — the ktest::internal seam functions
// runner.cpp defines, stood in host_shim.hpp — is shared with the suite's
// other file, so that both drive records the same way the booted runner
// does. The stage framing itself is the shared stage runner's
// (../src/run_stage.hpp): the same code the environment's default stages run
// on the kernel.
//
// This file includes ktest.hpp first, so the fixture bodies write the
// KTEST_-prefixed assertion spellings — the machinery under test — while the
// bare names stay gtest's for the TEST cases, which take no message argument.
// gtest.h redefining the bare names on arrival is therefore expected and
// silenced here alone.
#include <ktest/environment.hpp>
#include <ktest/ktest.hpp>

#include "../src/run_stage.hpp"
#include "host_shim.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <gtest/gtest.h>
#pragma clang diagnostic pop

#include <algorithm>
#include <array>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <hsien/runner.hpp>

namespace {

/// Scratch strings the too-many-suites case needs outliving its arrays.
std::vector<std::string> NAMES;
std::vector<std::string> SUITES;

/// Runs one suite hook the way the runner frames one: the shared stage
/// runner publishes the state and the context the hook's assertions record
/// into.
auto run_suite_hook(ktest::internal::SuiteHook const &hook) -> void {
    // The cases judge a hook by its log and its failure counts; the verdict
    // the stage runner answers with is theirs to derive.
    auto _ = ktest::internal::run_hook(hook.name, hook.invoke, REPORTER);
}

/// A selection that takes every record, for cases that hand a plan an
/// explicit range instead of the run's filter.
auto take_all(::hsien::TestRecord const &, usize const) -> bool {
    return true;
}

/// Gives a case an empty registry and puts back what it had afterwards,
/// so the cases cannot leak registrations into each other.
struct RegistryReset {
    ktest::internal::EnvironmentRegistry const saved = ktest::internal::environments();
    ~RegistryReset() {
        ktest::internal::environments() = saved;
    }
};

/// A test environment with a name in the log and an optional broken
/// SetUp, for the walk-order cases. A broken SetUp fails the way a real
/// one would: it records a failed check, which the stage framing turns
/// into the walk-stopping answer.
class NamedEnvironment: public ktest::Environment {
  public:
    bool setup_ok{true};

    NamedEnvironment(char const *const name = "", bool const setup_succeeds = true)
        : setup_ok(setup_succeeds), m_name(name) {}

    void SetUp() {
        LOG.push_back(std::string{m_name} + ".SetUp");
        if (!setup_ok) {
            KTEST_RECORD_MESSAGE("the environment could not come up");
        }
    }

    void TearDown() {
        LOG.push_back(std::string{m_name} + ".TearDown");
    }

  private:
    char const *m_name;
};

/// An environment that overrides nothing: both defaults run, which is
/// what a stage with no work of its own gets.
class SilentEnvironment: public ktest::Environment {};

/// An environment whose TearDown fails: the stage records a failed check,
/// which the default framing reports and the walk answers with.
class FailingTearDownEnvironment: public ktest::Environment {
  public:
    void TearDown() override {
        LOG.push_back("failing.TearDown");
        KTEST_RECORD_MESSAGE("the teardown could not undo the setup");
    }
};

// --- the fixtures and tests the lifecycle cases drive ---

/// The order case's fixture: SetUp writes the member the body reads, and
/// both ends of the lifecycle leave a mark.
class LifecycleFixture: public ktest::Fixture<LifecycleFixture> {
  public:
    usize value{};

    void SetUp() {
        LOG.push_back("setup");
        value = 42;
    }

    void TearDown() {
        LOG.push_back("teardown");
    }
};

KTEST_F(LifecycleFixture, Orderly) {
    LOG.push_back("body");
    KTEST_EXPECT_TRUE(this->value == 42, "SetUp wrote the member the body reads");
}

KTEST_F(LifecycleFixture, BodyFails) {
    LOG.push_back("body");
    KTEST_RECORD_MESSAGE("the case asked for one failed check");
}

KTEST_F(LifecycleFixture, BodyAsserts) {
    LOG.push_back("body");
    KTEST_ASSERT_TRUE(false, "the case asked for a fatal failure");
    LOG.push_back("body-continued");
}

KTEST_F(LifecycleFixture, BodySkips) {
    LOG.push_back("body");
    KTEST_SKIP("the case asked to be skipped");
}

KTEST_F(LifecycleFixture, Mutates) {
    LOG.push_back("body");
    this->value = 7;
}

KTEST_F(LifecycleFixture, GetsAFreshFixture) {
    LOG.push_back("body");
    KTEST_EXPECT_TRUE(this->value == 42, "the mutation the earlier test made is gone");
}

/// A fixture whose SetUp cannot build its state, and one whose SetUp
/// declares the machine unable to exercise the test at all.
class SetupFailsFixture: public ktest::Fixture<SetupFailsFixture> {
  public:
    void SetUp() {
        LOG.push_back("setup");
        KTEST_RECORD_MESSAGE("the setup could not build its state");
    }

    void TearDown() {
        LOG.push_back("teardown");
    }
};

bool SETUP_FAILS_BODY_RAN = false;

KTEST_F(SetupFailsFixture, BodyIsLeftOut) {
    LOG.push_back("body");
    SETUP_FAILS_BODY_RAN = true;
}

class SetupSkipsFixture: public ktest::Fixture<SetupSkipsFixture> {
  public:
    void SetUp() {
        LOG.push_back("setup");
        KTEST_SKIP("this machine cannot exercise it");
    }

    void TearDown() {
        LOG.push_back("teardown");
    }
};

bool SETUP_SKIPS_BODY_RAN = false;

KTEST_F(SetupSkipsFixture, BodyIsLeftOut) {
    LOG.push_back("body");
    SETUP_SKIPS_BODY_RAN = true;
}

/// A fixture whose TearDown fails after a passing body: the verdict the
/// runner builds must say FAILED all the same.
class TearDownFailsFixture: public ktest::Fixture<TearDownFailsFixture> {
  public:
    void TearDown() {
        LOG.push_back("teardown");
        KTEST_RECORD_MESSAGE("the teardown could not undo the setup");
    }
};

KTEST_F(TearDownFailsFixture, BodyPasses) {
    LOG.push_back("body");
}

/// A fixture that overrides only SetUp: the base TearDown is the no-op it
/// defaults to, which is what a fixture with nothing to undo gets.
class SetUpOnlyFixture: public ktest::Fixture<SetUpOnlyFixture> {
  public:
    void SetUp() {
        LOG.push_back("setup");
    }
};

KTEST_F(SetUpOnlyFixture, DefaultTearDownIsSilent) {
    LOG.push_back("body");
}

/// A fixture whose body and TearDown both fail: the context counts both,
/// which is what puts "2 of" in the runner's FAILED line.
class TwoFailuresFixture: public ktest::Fixture<TwoFailuresFixture> {
  public:
    void TearDown() {
        KTEST_RECORD_MESSAGE("the teardown failed too");
    }
};

KTEST_F(TwoFailuresFixture, BodyAndTearDownFail) {
    KTEST_RECORD_MESSAGE("the body failed");
}

// --- the suite the suite-hook cases drive ---

usize HOOKED_SUITE_SETUPS = 0;

class HookedFixture: public ktest::Fixture<HookedFixture> {
  public:
    void SetUp() {
        LOG.push_back("fixture-setup");
    }

    void TearDown() {
        LOG.push_back("fixture-teardown");
    }
};

KTEST_SUITE_SETUP(HookedFixture) {
    HOOKED_SUITE_SETUPS += 1;
    LOG.push_back("suite-setup");
}

KTEST_SUITE_TEARDOWN(HookedFixture) {
    LOG.push_back("suite-teardown");
}

KTEST_F(HookedFixture, First) {
    LOG.push_back("first");
}

KTEST_F(HookedFixture, Second) {
    LOG.push_back("second");
}

// --- the cases ---

TEST(KtestFixtureMacro, ExpansionShape) {
    // The expansion's pieces, checked the only way a host can: by name.
    static_assert(std::is_base_of_v<LifecycleFixture, LifecycleFixture_Orderly_Test>,
                  "the macro derives the test's class from the fixture");
    static_assert(std::is_base_of_v<ktest::Fixture<LifecycleFixture>, LifecycleFixture>,
                  "the fixture class roots itself in its own Fixture base");
    auto const &record = ktest_record_LifecycleFixture_Orderly;
    EXPECT_STREQ(record.name, "LifecycleFixture.Orderly");
    EXPECT_NE(record.invoke, nullptr);
    EXPECT_TRUE(std::string_view{record.location.file_name()}.ends_with("fixture_test.cpp"));
    EXPECT_EQ(FRAMEWORK_ERRORS, usize{0});
}

TEST(KtestFixtureLifecycle, SetUpRunsBeforeBodyAndTearDownAfter) {
    LOG.clear();
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_LifecycleFixture_Orderly);
    EXPECT_EQ(outcome.failures, usize{0});
    EXPECT_FALSE(outcome.skipped);
    ASSERT_EQ(LOG.size(), usize{3});
    EXPECT_EQ(LOG[0], "setup");
    EXPECT_EQ(LOG[1], "body");
    EXPECT_EQ(LOG[2], "teardown");
}

TEST(KtestFixtureLifecycle, TearDownRunsAfterAFailedCheck) {
    LOG.clear();
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_LifecycleFixture_BodyFails);
    EXPECT_EQ(outcome.failures, usize{1});
    EXPECT_EQ(LOG.size(), usize{3});
    EXPECT_EQ(LOG[2], "teardown");
    ASSERT_EQ(FAILURES.size(), usize{1});
    EXPECT_NE(FAILURES[0].find("the case asked for one failed check"), std::string::npos);
}

TEST(KtestFixtureLifecycle, TearDownRunsAfterAnAssertReturned) {
    LOG.clear();
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_LifecycleFixture_BodyAsserts);
    EXPECT_EQ(outcome.failures, usize{1});
    // The assert returned out of the body, so nothing after it ran, and
    // the teardown ran anyway.
    EXPECT_EQ(LOG.size(), usize{3});
    EXPECT_EQ(LOG[2], "teardown");
}

TEST(KtestFixtureLifecycle, SkipInTheBodyStillTearsDown) {
    LOG.clear();
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_LifecycleFixture_BodySkips);
    EXPECT_EQ(outcome.failures, usize{0});
    EXPECT_TRUE(outcome.skipped);
    EXPECT_EQ(LOG.size(), usize{3});
    EXPECT_EQ(LOG[2], "teardown");
}

TEST(KtestFixtureLifecycle, SetUpFailureLeavesTheBodyOut) {
    LOG.clear();
    FAILURES.clear();
    SETUP_FAILS_BODY_RAN = false;
    auto const outcome = run_record(ktest_record_SetupFailsFixture_BodyIsLeftOut);
    // No body runs against state that was not built; the teardown undoes
    // what little the setup did.
    EXPECT_EQ(outcome.failures, usize{1});
    EXPECT_FALSE(outcome.skipped);
    EXPECT_FALSE(SETUP_FAILS_BODY_RAN);
    EXPECT_EQ(LOG.size(), usize{2});
    EXPECT_EQ(LOG[1], "teardown");
}

TEST(KtestFixtureLifecycle, SetUpSkipLeavesTheBodyOut) {
    LOG.clear();
    FAILURES.clear();
    SETUP_SKIPS_BODY_RAN = false;
    auto const outcome = run_record(ktest_record_SetupSkipsFixture_BodyIsLeftOut);
    EXPECT_EQ(outcome.failures, usize{0});
    EXPECT_TRUE(outcome.skipped);
    EXPECT_FALSE(SETUP_SKIPS_BODY_RAN);
    EXPECT_EQ(LOG.size(), usize{2});
    EXPECT_EQ(LOG[1], "teardown");
}

TEST(KtestFixtureLifecycle, TearDownFailureFailsAPassingBody) {
    LOG.clear();
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_TearDownFailsFixture_BodyPasses);
    // The teardown's failure is the test's; the verdict says FAILED.
    EXPECT_EQ(outcome.failures, usize{1});
    ASSERT_EQ(LOG.size(), usize{2});
    EXPECT_EQ(LOG[1], "teardown");
}

TEST(KtestFixtureLifecycle, CountingCoversTheWholeLifecycle) {
    LOG.clear();
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_TwoFailuresFixture_BodyAndTearDownFail);
    // Body and teardown land in one context, so the FAILED line can say
    // how many of the run's checks failed.
    EXPECT_EQ(outcome.failures, usize{2});
    EXPECT_EQ(FAILURES.size(), usize{2});
}

TEST(KtestFixtureLifecycle, EachTestGetsAFreshFixture) {
    LOG.clear();
    FAILURES.clear();
    auto const first = run_record(ktest_record_LifecycleFixture_Mutates);
    EXPECT_EQ(first.failures, usize{0});
    auto const second = run_record(ktest_record_LifecycleFixture_GetsAFreshFixture);
    EXPECT_EQ(second.failures, usize{0});
}

TEST(KtestFixtureLifecycle, SetUpOnlyFixtureKeepsTheDefaultTearDown) {
    LOG.clear();
    FAILURES.clear();
    auto const outcome = run_record(ktest_record_SetUpOnlyFixture_DefaultTearDownIsSilent);
    EXPECT_EQ(outcome.failures, usize{0});
    ASSERT_EQ(LOG.size(), usize{2});
    EXPECT_EQ(LOG[0], "setup");
    EXPECT_EQ(LOG[1], "body");
}

TEST(KtestFixtureSuite, SetupRunsOnceAroundTheSuitesTests) {
    LOG.clear();
    FAILURES.clear();
    HOOKED_SUITE_SETUPS = 0;

    ::hsien::TestRecord const records[] = {ktest_record_HookedFixture_First,
                                           ktest_record_HookedFixture_Second};
    ktest::internal::SuiteHook const hooks[] = {ktest_suite_setup_HookedFixture,
                                                ktest_suite_teardown_HookedFixture};
    auto plan =
        ktest::internal::SuitePlan{hooks + 0, hooks + 2, records + 0, records + 2, take_all};
    ASSERT_TRUE(plan.valid());

    // The run loop's ask-sequence, over the plan, with the real hooks.
    auto *const setup = plan.before(ktest_record_HookedFixture_First.name);
    ASSERT_TRUE(setup != nullptr);
    run_suite_hook(*setup);
    run_record(ktest_record_HookedFixture_First);
    EXPECT_TRUE(plan.after(ktest_record_HookedFixture_First.name) == nullptr);
    // The suite setup runs once, not per test.
    EXPECT_TRUE(plan.before(ktest_record_HookedFixture_Second.name) == nullptr);
    run_record(ktest_record_HookedFixture_Second);
    auto *const teardown = plan.after(ktest_record_HookedFixture_Second.name);
    ASSERT_TRUE(teardown != nullptr);
    run_suite_hook(*teardown);

    EXPECT_EQ(HOOKED_SUITE_SETUPS, usize{1});
    ASSERT_EQ(LOG.size(), usize{8});
    EXPECT_EQ(LOG[0], "suite-setup");
    EXPECT_EQ(LOG[1], "fixture-setup");
    EXPECT_EQ(LOG[2], "first");
    EXPECT_EQ(LOG[3], "fixture-teardown");
    EXPECT_EQ(LOG[4], "fixture-setup");
    EXPECT_EQ(LOG[5], "second");
    EXPECT_EQ(LOG[6], "fixture-teardown");
    EXPECT_EQ(LOG[7], "suite-teardown");
}

TEST(KtestFixtureSuite, PoisonedSuiteKeepsItsTeardown) {
    LOG.clear();
    FAILURES.clear();
    HOOKED_SUITE_SETUPS = 0;

    ::hsien::TestRecord const records[] = {ktest_record_HookedFixture_First,
                                           ktest_record_HookedFixture_Second};
    ktest::internal::SuiteHook const hooks[] = {ktest_suite_setup_HookedFixture,
                                                ktest_suite_teardown_HookedFixture};
    auto plan =
        ktest::internal::SuitePlan{hooks + 0, hooks + 2, records + 0, records + 2, take_all};
    ASSERT_TRUE(plan.valid());

    // A failed setup poisons the suite: the runner reports each of its
    // tests without running it, and the teardown still unwinds after the
    // last of them. The walk below is the run loop's ask-sequence.
    auto *const setup = plan.before(ktest_record_HookedFixture_First.name);
    ASSERT_TRUE(setup != nullptr);
    plan.poison(ktest_record_HookedFixture_First.name);
    EXPECT_TRUE(plan.poisoned(ktest_record_HookedFixture_Second.name));
    // The suite setup is asked about once; both tests report failed.
    EXPECT_TRUE(plan.before(ktest_record_HookedFixture_Second.name) == nullptr);
    EXPECT_TRUE(plan.after(ktest_record_HookedFixture_First.name) == nullptr);
    EXPECT_TRUE(plan.after(ktest_record_HookedFixture_Second.name) != nullptr);
    EXPECT_EQ(HOOKED_SUITE_SETUPS, usize{0});
    EXPECT_TRUE(LOG.empty());
}

TEST(KtestFixtureSuitePlan, HooksFireOncePerSuiteAcrossDisjointRecords) {
    auto const nowhere = std::source_location::current();
    // SuiteA's tests are disjoint in the section order; SuiteC has a name
    // no hook names.
    ::hsien::TestRecord const records[] = {
        {"SuiteA.T1", nullptr, nowhere},
        {"SuiteB.T1", nullptr, nowhere},
        {"SuiteA.T2", nullptr, nowhere},
        {"SuiteC", nullptr, nowhere},
    };
    // The plan only reads these records and hooks, so a null invoke is
    // fine: nothing here calls one.
    ktest::internal::SuiteHook const hooks[] = {
        {"SuiteA.SuiteSetUp", "SuiteA", ktest::internal::SuiteHookKind::SetUp, nullptr},
        {"SuiteA.SuiteTearDown", "SuiteA", ktest::internal::SuiteHookKind::TearDown, nullptr},
        {"SuiteB.SuiteSetUp", "SuiteB", ktest::internal::SuiteHookKind::SetUp, nullptr},
        {"SuiteB.SuiteTearDown", "SuiteB", ktest::internal::SuiteHookKind::TearDown, nullptr},
    };
    auto plan =
        ktest::internal::SuitePlan{hooks + 0, hooks + 4, records + 0, records + 4, take_all};
    ASSERT_TRUE(plan.valid());

    EXPECT_TRUE(plan.before("SuiteA.T1") == &hooks[0]);
    EXPECT_TRUE(plan.after("SuiteA.T1") == nullptr);
    EXPECT_TRUE(plan.before("SuiteB.T1") == &hooks[2]);
    EXPECT_TRUE(plan.after("SuiteB.T1") == &hooks[3]);
    // SuiteA's setup already ran.
    EXPECT_TRUE(plan.before("SuiteA.T2") == nullptr);
    // SuiteA.T2 is its suite's last test of the run.
    EXPECT_TRUE(plan.after("SuiteA.T2") == &hooks[1]);
    EXPECT_TRUE(plan.before("SuiteC") == nullptr);
    EXPECT_TRUE(plan.after("SuiteC") == nullptr);
}

TEST(KtestFixtureSuitePlan, SuiteNamesMatchWholeSegments) {
    auto const nowhere = std::source_location::current();
    ::hsien::TestRecord const records[] = {
        {"Foo.T1", nullptr, nowhere},
        {"FooBar.T1", nullptr, nowhere},
    };
    ktest::internal::SuiteHook const hooks[] = {
        {"Foo.SuiteSetUp", "Foo", ktest::internal::SuiteHookKind::SetUp, nullptr},
    };
    auto plan =
        ktest::internal::SuitePlan{hooks + 0, hooks + 1, records + 0, records + 2, take_all};
    ASSERT_TRUE(plan.valid());
    EXPECT_TRUE(plan.before("Foo.T1") == &hooks[0]);
    // A suite named Foo does not pick up a suite named FooBar's tests.
    EXPECT_TRUE(plan.before("FooBar.T1") == nullptr);
}

TEST(KtestFixtureSuitePlan, SelectionLeavesOtherSuitesUntouched) {
    auto const nowhere = std::source_location::current();
    ::hsien::TestRecord const records[] = {
        {"SuiteA.T1", nullptr, nowhere},
        {"SuiteB.T1", nullptr, nowhere},
    };
    ktest::internal::SuiteHook const hooks[] = {
        {"SuiteA.SuiteSetUp", "SuiteA", ktest::internal::SuiteHookKind::SetUp, nullptr},
        {"SuiteB.SuiteSetUp", "SuiteB", ktest::internal::SuiteHookKind::SetUp, nullptr},
    };
    auto const only_a = [](::hsien::TestRecord const &record, usize const) {
        return std::string_view{record.name}.starts_with("SuiteA.");
    };
    auto plan = ktest::internal::SuitePlan{hooks + 0, hooks + 2, records + 0, records + 2, only_a};
    ASSERT_TRUE(plan.valid());
    EXPECT_TRUE(plan.before("SuiteA.T1") == &hooks[0]);
    // A suite with no teardown hook has none to run.
    EXPECT_TRUE(plan.after("SuiteA.T1") == nullptr);
    EXPECT_TRUE(plan.before("SuiteB.T1") == nullptr);
    EXPECT_FALSE(plan.poisoned("SuiteB.T1"));
}

TEST(KtestFixtureSuitePlan, DuplicateHookIsReportedNotGuessed) {
    auto const nowhere = std::source_location::current();
    ::hsien::TestRecord const records[] = {
        {"SuiteA.T1", nullptr, nowhere},
    };
    ktest::internal::SuiteHook const hooks[] = {
        {"SuiteA.SuiteSetUp", "SuiteA", ktest::internal::SuiteHookKind::SetUp, nullptr},
        {"SuiteA.SuiteSetUpAgain", "SuiteA", ktest::internal::SuiteHookKind::SetUp, nullptr},
    };
    auto plan =
        ktest::internal::SuitePlan{hooks + 0, hooks + 2, records + 0, records + 1, take_all};
    // Two setups for one suite would run unpredictably.
    EXPECT_FALSE(plan.valid());
}

TEST(KtestFixtureSuitePlan, MoreSuitesThanTheTableTracksIsReported) {
    auto const nowhere = std::source_location::current();
    auto const count = ktest::internal::SuitePlan::MAX_SUITES + 1;
    std::array<::hsien::TestRecord, count> records{};
    std::array<ktest::internal::SuiteHook, count> hooks{};
    for (auto i = usize{0}; i < count; ++i) {
        NAMES.push_back("Suite" + std::to_string(i) + ".T1");
        SUITES.push_back("Suite" + std::to_string(i));
        records[i] = {NAMES.back().c_str(), nullptr, nowhere};
        hooks[i] = {SUITES.back().c_str(), SUITES.back().c_str(),
                    ktest::internal::SuiteHookKind::SetUp, nullptr};
    }
    auto plan =
        ktest::internal::SuitePlan{hooks.data(), hooks.data() + hooks.size(), records.data(),
                                   records.data() + records.size(), take_all};
    // One suite more than the table tracks is an authoring bug the run
    // reports, not a truncation.
    EXPECT_FALSE(plan.valid());
}

TEST(KtestEnvironment, RegistrationKeepsOrder) {
    auto reset = RegistryReset{};
    NamedEnvironment a{"a"};
    NamedEnvironment b{"b"};
    NamedEnvironment c{"c"};
    EXPECT_TRUE(ktest::add_environment(a));
    EXPECT_TRUE(ktest::add_environment(b));
    EXPECT_TRUE(ktest::add_environment(c));
    auto &registry = ktest::internal::environments();
    ASSERT_EQ(registry.count, usize{3});
    EXPECT_TRUE(registry.entries[0] == &a);
    EXPECT_TRUE(registry.entries[1] == &b);
    EXPECT_TRUE(registry.entries[2] == &c);
    EXPECT_EQ(registry.dropped, usize{0});
}

TEST(KtestEnvironment, FullRegistryDropsAndCountsTheDrop) {
    auto reset = RegistryReset{};
    std::array<NamedEnvironment, ktest::MAX_ENVIRONMENTS + 1> environments{};
    for (auto i = usize{0}; i < ktest::MAX_ENVIRONMENTS; ++i) {
        EXPECT_TRUE(ktest::add_environment(environments[i]));
    }
    EXPECT_FALSE(ktest::add_environment(environments.back()));
    // The drop is visible to the run, which reports it rather than
    // passing without the stage.
    EXPECT_EQ(ktest::internal::environments().dropped, usize{1});
}

TEST(KtestEnvironment, SetUpRunsForwardTeardownReverses) {
    auto reset = RegistryReset{};
    LOG.clear();
    FAILURES.clear();
    NamedEnvironment a{"a"};
    NamedEnvironment b{"b"};
    NamedEnvironment c{"c"};
    ASSERT_TRUE(ktest::add_environment(a));
    ASSERT_TRUE(ktest::add_environment(b));
    ASSERT_TRUE(ktest::add_environment(c));

    auto run = ktest::internal::EnvironmentRun{};
    EXPECT_TRUE(run.set_up(REPORTER));
    run.tear_down(REPORTER);

    ASSERT_EQ(LOG.size(), usize{6});
    EXPECT_EQ(LOG[0], "a.SetUp");
    EXPECT_EQ(LOG[1], "b.SetUp");
    EXPECT_EQ(LOG[2], "c.SetUp");
    EXPECT_EQ(LOG[3], "c.TearDown");
    EXPECT_EQ(LOG[4], "b.TearDown");
    EXPECT_EQ(LOG[5], "a.TearDown");
}

TEST(KtestEnvironment, FailedSetUpStopsAndTeardownUnwindsThePrefix) {
    auto reset = RegistryReset{};
    LOG.clear();
    FAILURES.clear();
    NamedEnvironment a{"a"};
    NamedEnvironment b{"b", false};
    NamedEnvironment c{"c"};
    ASSERT_TRUE(ktest::add_environment(a));
    ASSERT_TRUE(ktest::add_environment(b));
    ASSERT_TRUE(ktest::add_environment(c));

    auto run = ktest::internal::EnvironmentRun{};
    // The run learns the stage never came up: the failed check the broken
    // SetUp recorded is the walk-stopping answer.
    EXPECT_FALSE(run.set_up(REPORTER));
    run.tear_down(REPORTER);

    ASSERT_EQ(LOG.size(), usize{4});
    EXPECT_EQ(LOG[0], "a.SetUp");
    EXPECT_EQ(LOG[1], "b.SetUp");
    EXPECT_EQ(LOG[2], "b.TearDown");
    EXPECT_EQ(LOG[3], "a.TearDown");
    // No environment after the failed one is set up.
    EXPECT_TRUE(std::find(LOG.begin(), LOG.end(), "c.SetUp") == LOG.end());
}

TEST(KtestEnvironment, NothingRegisteredIsAnEmptyRun) {
    auto reset = RegistryReset{};
    LOG.clear();
    auto run = ktest::internal::EnvironmentRun{};
    EXPECT_TRUE(run.set_up(REPORTER));
    run.tear_down(REPORTER);
    EXPECT_TRUE(LOG.empty());
}

TEST(KtestEnvironment, AnEnvironmentMayOverrideNothing) {
    auto reset = RegistryReset{};
    LOG.clear();
    FAILURES.clear();
    NamedEnvironment named{"named"};
    SilentEnvironment silent;
    ASSERT_TRUE(ktest::add_environment(named));
    ASSERT_TRUE(ktest::add_environment(silent));

    auto run = ktest::internal::EnvironmentRun{};
    EXPECT_TRUE(run.set_up(REPORTER));
    run.tear_down(REPORTER);

    // The silent environment takes both defaults: nothing logged from it,
    // and the walk no different for it.
    ASSERT_EQ(LOG.size(), usize{2});
    EXPECT_EQ(LOG[0], "named.SetUp");
    EXPECT_EQ(LOG[1], "named.TearDown");
}

TEST(KtestEnvironment, FailedTearDownIsReportedAndTheUnwindContinues) {
    auto reset = RegistryReset{};
    LOG.clear();
    FAILURES.clear();
    auto const framework_errors_before = FRAMEWORK_ERRORS;
    NamedEnvironment a{"a"};
    FailingTearDownEnvironment failing;
    ASSERT_TRUE(ktest::add_environment(a));
    ASSERT_TRUE(ktest::add_environment(failing));

    auto run = ktest::internal::EnvironmentRun{};
    EXPECT_TRUE(run.set_up(REPORTER));
    run.tear_down(REPORTER);

    // The unwind reaches every environment despite the failure, in reverse.
    ASSERT_EQ(LOG.size(), usize{3});
    EXPECT_EQ(LOG[0], "a.SetUp");
    EXPECT_EQ(LOG[1], "failing.TearDown");
    EXPECT_EQ(LOG[2], "a.TearDown");
    // The failed teardown reaches the framework-error channel — the one
    // error line the booted runner's console carries — beside the check it
    // recorded. The count is put back: a well-behaved case causes no
    // framework error, which is what the suite's other cases assert.
    ASSERT_EQ(FRAMEWORK_ERRORS, framework_errors_before + 1);
    ASSERT_EQ(FAILURES.size(), usize{2});
    EXPECT_NE(FAILURES.back().find("framework: the environment teardown failed"),
              std::string::npos);
    FRAMEWORK_ERRORS = framework_errors_before;
}

} // namespace
