/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The instantiation half of the interface, on the host: the records a
// KTEST_P/KTEST_F_P/KTEST_T declares, the `/N` names they carry, the value
// each record's body sees, the fixture lifecycle a KTEST_F_P instantiation
// runs, and the filter and summary treatment the runner gives them — all
// against the real macro expansions driven through hsien's own record loop.
//
// The runner's kernel-only half stands in host_shim.hpp, shared with the
// fixture file, which is the seam the booted runner implements.
//
// This file includes ktest.hpp first, so the instantiation bodies write the
// KTEST_-prefixed assertion spellings — the machinery under test — while the
// bare names stay gtest's for the TEST cases, which take no message argument.
// gtest.h redefining the bare names on arrival is therefore expected and
// silenced here alone.
#include <ktest/filter.hpp>
#include <ktest/ktest.hpp>
#include <ktest/summary.hpp>

#include "host_shim.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <gtest/gtest.h>
#pragma clang diagnostic pop

#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <hsien/runner.hpp>

namespace {

// --- the value-parameterized suite the macro cases drive ---

/// The named table and the KTEST_P family over it: three values, one record
/// each, whose bodies log the value they were built with.
KTEST_VALUE_PARAM(Numbers, int, Values, 1, 42, 99);

KTEST_P(Numbers, ReadsItsValue) {
    LOG.push_back("Numbers.ReadsItsValue:" + std::to_string(GetParam()));
}

/// A body that only the middle value's record can pass, so the verdict cases
/// have a family whose records disagree on purpose. The KTEST_ spellings are
/// the ones the bodies write on the host: gtest's bare names take the common
/// ones over after its include.
KTEST_P(Numbers, FailsWhereAsked) {
    if (GetParam() != 42) {
        KTEST_RECORD_MESSAGE("only the middle value passes");
    }
}

// --- the fixture-composed suite ---

/// The fixture the KTEST_F_P cases drive: SetUp writes a member the body
/// reads beside its table value, and a member SetUp leaves alone proves the
/// object is rebuilt per record.
class ParamsFixture: public ktest::Fixture<ParamsFixture> {
  public:
    int built_in_setup{};
    int left_alone{5};

    void SetUp() {
        LOG.push_back("setup");
        built_in_setup = 7;
    }

    void TearDown() {
        LOG.push_back("teardown");
    }
};

KTEST_VALUE_PARAM(ParamsFixture, int, FixtureValues, 3, 4);

KTEST_F_P(ParamsFixture, SeesFixtureAndParameter) {
    LOG.push_back("body:" + std::to_string(this->built_in_setup) + ":" +
                  std::to_string(GetParam()));
}

KTEST_F_P(ParamsFixture, MutatesAndLeaves) {
    LOG.push_back("mutate:" + std::to_string(this->left_alone));
    this->left_alone = 99;
}

// --- the typed suite ---

/// The types the KTEST_T cases drive: each carries a tag the body logs, so a
/// case can check which type each record was built with.
class ByteBox {
  public:
    static constexpr int TAG = 1;
};

class IntBox {
  public:
    static constexpr int TAG = 2;
};

class LongBox {
  public:
    static constexpr int TAG = 3;
};

KTEST_TYPE_LIST(Boxes, (ByteBox, IntBox, LongBox));

KTEST_T(Boxes, ReadsItsType) {
    LOG.push_back("Boxes.ReadsItsType:" + std::to_string(T::TAG));
}

KTEST_T(Boxes, ChecksItsType) {
    KTEST_EXPECT_TRUE(sizeof(T) >= alignof(T), "the type's layout is sane");
}

// --- the cases ---

TEST(KtestParamMacro, ExpansionShape) {
    // The expansion's pieces, checked the only way a host can: by name.
    static_assert(
        std::is_base_of_v<ktest::Fixture<Numbers_ReadsItsValue_Test>, Numbers_ReadsItsValue_Test>,
        "the macro derives the test's class from its own Fixture base");
    static_assert(std::is_base_of_v<Numbers_ParamBase, Numbers_ReadsItsValue_Test>,
                  "the macro derives the test's class from the suite's param base");
    static_assert(sizeof(ktest_param_storage_Numbers_ReadsItsValue) >=
                      sizeof(Numbers_ReadsItsValue_Test),
                  "the macro reserves storage that fits the test's object");
    ASSERT_EQ(Numbers_ParamBase::COUNT, usize{3});
    auto const &block = ktest_param_block_Numbers_ReadsItsValue;
    EXPECT_STREQ(block.records[0].name, "Numbers.ReadsItsValue/0");
    EXPECT_STREQ(block.records[1].name, "Numbers.ReadsItsValue/1");
    EXPECT_STREQ(block.records[2].name, "Numbers.ReadsItsValue/2");
    EXPECT_NE(block.records[0].invoke, nullptr);
    EXPECT_TRUE(
        std::string_view{block.records[0].location.file_name()}.ends_with("instantiate_test.cpp"));
    EXPECT_EQ(FRAMEWORK_ERRORS, usize{0});
}

TEST(KtestParamMacro, TableIsTheNamedOne) {
    // The table the suite's records iterate is the array KTEST_VALUE_PARAM
    // named, reached through the base class it declares.
    EXPECT_EQ(sizeof(Values) / sizeof(Values[0]), usize{3});
    EXPECT_EQ(Values[1], 42);
    EXPECT_EQ(Numbers_ParamBase::VALUES[1], 42);
    static_assert(std::is_same_v<Numbers_ParamBase::ParamType, int>,
                  "the base carries the table's type");
}

TEST(KtestParamIteration, EachRecordGetsItsOwnValue) {
    LOG.clear();
    FAILURES.clear();
    auto const &block = ktest_param_block_Numbers_ReadsItsValue;
    for (auto i = usize{0}; i < Numbers_ParamBase::COUNT; ++i) {
        auto const outcome = run_record(block.records[i]);
        EXPECT_EQ(outcome.failures, usize{0});
        EXPECT_FALSE(outcome.skipped);
    }
    ASSERT_EQ(LOG.size(), usize{3});
    EXPECT_EQ(LOG[0], "Numbers.ReadsItsValue:1");
    EXPECT_EQ(LOG[1], "Numbers.ReadsItsValue:42");
    EXPECT_EQ(LOG[2], "Numbers.ReadsItsValue:99");
}

TEST(KtestParamIteration, TheWholeBlockIsTheWholeFamily) {
    // The block one KTEST_P declares is exactly its records, contiguous: the
    // section walk of the booted runner depends on it.
    ASSERT_EQ(Numbers_ParamBase::COUNT, usize{3});
    auto const &block = ktest_param_block_Numbers_ReadsItsValue;
    EXPECT_STREQ(block.records[0].name, "Numbers.ReadsItsValue/0");
    EXPECT_TRUE(&block.records[2] == &block.records[0] + 2);
}

TEST(KtestParamLifecycle, FixtureWrapsEveryRecord) {
    LOG.clear();
    FAILURES.clear();
    auto const &block = ktest_param_block_ParamsFixture_SeesFixtureAndParameter;
    ASSERT_EQ(ParamsFixture_ParamBase::COUNT, usize{2});
    for (auto i = usize{0}; i < ParamsFixture_ParamBase::COUNT; ++i) {
        auto const outcome = run_record(block.records[i]);
        EXPECT_EQ(outcome.failures, usize{0});
    }
    // SetUp, the body with the fixture's member and the record's own value,
    // TearDown — per record, not once per family.
    ASSERT_EQ(LOG.size(), usize{6});
    EXPECT_EQ(LOG[0], "setup");
    EXPECT_EQ(LOG[1], "body:7:3");
    EXPECT_EQ(LOG[2], "teardown");
    EXPECT_EQ(LOG[3], "setup");
    EXPECT_EQ(LOG[4], "body:7:4");
    EXPECT_EQ(LOG[5], "teardown");
}

TEST(KtestParamLifecycle, EachRecordGetsAFreshFixture) {
    LOG.clear();
    FAILURES.clear();
    auto const &block = ktest_param_block_ParamsFixture_MutatesAndLeaves;
    for (auto i = usize{0}; i < ParamsFixture_ParamBase::COUNT; ++i) {
        auto const outcome = run_record(block.records[i]);
        EXPECT_EQ(outcome.failures, usize{0});
    }
    // The mutation one record made is gone by the next: a fresh object per
    // record, as for KTEST_F.
    ASSERT_EQ(LOG.size(), usize{6});
    EXPECT_EQ(LOG[1], "mutate:5");
    EXPECT_EQ(LOG[4], "mutate:5");
}

TEST(KtestParamVerdict, FailuresCountPerRecord) {
    LOG.clear();
    FAILURES.clear();
    auto const &block = ktest_param_block_Numbers_FailsWhereAsked;
    auto summary = ktest::Summary{};
    for (auto i = usize{0}; i < Numbers_ParamBase::COUNT; ++i) {
        auto const outcome = run_record(block.records[i]);
        // The runner's verdict: a record with failed checks counts as failed,
        // the rest as passed; instantiations are ordinary entries.
        summary.record(outcome.failures != 0, outcome.skipped);
        if (outcome.failures != 0) {
            ASSERT_EQ(FAILURES.size(), usize{1});
            EXPECT_NE(FAILURES[0].find("only the middle value passes"), std::string::npos);
        }
        FAILURES.clear();
    }
    EXPECT_EQ(summary.ran, usize{3});
    EXPECT_EQ(summary.passed, usize{1});
    EXPECT_EQ(summary.failed, usize{2});
    EXPECT_EQ(summary.skipped, usize{0});
}

TEST(KtestParamFilter, SuffixesMatchWholeNames) {
    // The filters are the runner's own matcher over the full name, `/N`
    // included: a suite glob picks a family up, an exact name picks one
    // instantiation out, and nothing bleeds between neighbours.
    EXPECT_TRUE(ktest::matches_filter("Numbers.ReadsItsValue/2", "Numbers.*"));
    EXPECT_TRUE(ktest::matches_filter("Numbers.ReadsItsValue/2", "Numbers.ReadsItsValue/2"));
    EXPECT_TRUE(ktest::matches_filter("Numbers.ReadsItsValue/2", "*ReadsItsValue/*"));
    EXPECT_FALSE(ktest::matches_filter("Numbers.ReadsItsValue/2", "Numbers.ReadsItsValue/1"));
    EXPECT_FALSE(ktest::matches_filter("Numbers.ReadsItsValue/20", "Numbers.ReadsItsValue/2"));
    EXPECT_FALSE(
        ktest::matches_filter("Numbers.ReadsItsValue/2", "Numbers.*:-Numbers.ReadsItsValue/2"));
    EXPECT_TRUE(ktest::matches_filter("Boxes.ReadsItsType/1", "Boxes.*"));
}

TEST(KtestTypedMacro, ExpansionShape) {
    static_assert(Boxes_TypeList::COUNT == 3, "the list carries all its types");
    ASSERT_EQ(Boxes_TypeList::COUNT, usize{3});
    auto const &block = ktest_typed_block_Boxes_ReadsItsType;
    EXPECT_STREQ(block.records[0].name, "Boxes.ReadsItsType/0");
    EXPECT_STREQ(block.records[1].name, "Boxes.ReadsItsType/1");
    EXPECT_STREQ(block.records[2].name, "Boxes.ReadsItsType/2");
    EXPECT_NE(block.records[0].invoke, nullptr);
    EXPECT_TRUE(
        std::string_view{block.records[0].location.file_name()}.ends_with("instantiate_test.cpp"));
    EXPECT_EQ(FRAMEWORK_ERRORS, usize{0});
}

TEST(KtestTypedIteration, EachRecordGetsItsOwnType) {
    LOG.clear();
    FAILURES.clear();
    auto const &block = ktest_typed_block_Boxes_ReadsItsType;
    for (auto i = usize{0}; i < Boxes_TypeList::COUNT; ++i) {
        auto const outcome = run_record(block.records[i]);
        EXPECT_EQ(outcome.failures, usize{0});
        EXPECT_FALSE(outcome.skipped);
    }
    // The list's order is the records' order: position i runs type i.
    ASSERT_EQ(LOG.size(), usize{3});
    EXPECT_EQ(LOG[0], "Boxes.ReadsItsType:1");
    EXPECT_EQ(LOG[1], "Boxes.ReadsItsType:2");
    EXPECT_EQ(LOG[2], "Boxes.ReadsItsType:3");
}

TEST(KtestTypedIteration, ChecksReachTheTypes) {
    LOG.clear();
    FAILURES.clear();
    auto const &block = ktest_typed_block_Boxes_ChecksItsType;
    for (auto i = usize{0}; i < Boxes_TypeList::COUNT; ++i) {
        auto const outcome = run_record(block.records[i]);
        EXPECT_EQ(outcome.failures, usize{0});
    }
    EXPECT_EQ(FRAMEWORK_ERRORS, usize{0});
}

TEST(KtestTypedVerdict, SummaryCountsInstantiations) {
    LOG.clear();
    FAILURES.clear();
    // The runner's SUMMARY adds up instantiations like any other entry: the
    // typed family all passing, the value family one failing of three.
    auto summary = ktest::Summary{};
    auto const &typed = ktest_typed_block_Boxes_ChecksItsType;
    for (auto i = usize{0}; i < Boxes_TypeList::COUNT; ++i) {
        auto const outcome = run_record(typed.records[i]);
        summary.record(outcome.failures != 0, outcome.skipped);
    }
    auto const &values = ktest_param_block_Numbers_FailsWhereAsked;
    for (auto i = usize{0}; i < Numbers_ParamBase::COUNT; ++i) {
        auto const outcome = run_record(values.records[i]);
        summary.record(outcome.failures != 0, outcome.skipped);
    }
    EXPECT_EQ(summary.ran, usize{6});
    EXPECT_EQ(summary.passed, usize{4});
    EXPECT_EQ(summary.failed, usize{2});
    EXPECT_EQ(summary.skipped, usize{0});
}

} // namespace
