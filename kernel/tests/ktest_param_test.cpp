/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The parameterized and typed tests' booted smoke: KTEST_P over a three-value
// table whose bodies check the value they were built with, KTEST_F_P proving
// the parameter composes with a fixture's SetUp, and KTEST_T over a two-type
// list whose body checks through T. Deliberately small: the frame's own
// proof, not a model for subsystem tests.
#include <ktest/ktest.hpp>

namespace {

// The named value table and the KTEST_P family over it: one record per
// value, each body referencing the record's own parameter.
KTEST_VALUE_PARAM(KtestParam, int, Sizes, 1, 42, 99);

KTEST_P(KtestParam, ValueIsPositive) {
    EXPECT_GT(GetParam(), 0, "the record's own table value");
}

KTEST_P(KtestParam, ValueIsOneOfTheTables) {
    EXPECT_TRUE(GetParam() == 1 || GetParam() == 42 || GetParam() == 99);
}

// The parameter composes with a fixture: the object derives from the fixture
// and the suite's param base, so one instantiation sees both.
class KtestParamFixture: public ktest::Fixture {
  public:
    usize built_in_setup{};

    void SetUp() override {
        built_in_setup = 7;
    }
};

KTEST_VALUE_PARAM(KtestParamFixture, int, FixtureValues, 5, 6);

KTEST_F_P(KtestParamFixture, SeesFixtureAndParameter) {
    EXPECT_EQ(this->built_in_setup, usize{7}, "the fixture's SetUp ran for this instantiation");
    EXPECT_TRUE(GetParam() > 0);
}

// The typed list and the KTEST_T family over it: one record per type, the
// body reading the record's own type as T. Each box carries a payload whose
// size the type's constant names, so the check is per type and real.
class ByteBox {
  public:
    static constexpr usize SIZE = 1;
    unsigned char payload;
};

class IntBox {
  public:
    static constexpr usize SIZE = 4;
    unsigned int payload;
};

KTEST_TYPE_LIST(KtestTyped, (ByteBox, IntBox));

KTEST_T(KtestTyped, SizeIsKnown) {
    EXPECT_EQ(sizeof(T), T::SIZE, "the record's own type is the body's T");
    EXPECT_GE(alignof(T), usize{1});
}

} // namespace
