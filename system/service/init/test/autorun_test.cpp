/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// init::AutorunCommand::parse(), the `+` splitting of `ours.autorun.boot`,
/// on the host: which words a value becomes, and which values it refuses.
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <init/autorun.hpp>

namespace {
using init::AutorunCommand;
using ours::Status;

auto words(AutorunCommand const &command) -> std::vector<std::string_view> {
    return {command.words().begin(), command.words().end()};
}

TEST(Autorun, ANameAloneIsItsOwnArgv) {
    AutorunCommand command;
    ASSERT_EQ(AutorunCommand::parse("fault_probe", &command), Status::Ok);
    EXPECT_EQ(command.name(), "fault_probe");
    EXPECT_EQ(words(command), (std::vector<std::string_view>{"fault_probe"}));
}

TEST(Autorun, PlusSeparatesArguments) {
    AutorunCommand command;
    ASSERT_EQ(AutorunCommand::parse("fault_probe+none+x", &command), Status::Ok);
    EXPECT_EQ(command.name(), "fault_probe");
    EXPECT_EQ(words(command), (std::vector<std::string_view>{"fault_probe", "none", "x"}));
}

TEST(Autorun, EmptyWordsAreSkipped) {
    AutorunCommand command;
    ASSERT_EQ(AutorunCommand::parse("+fault_probe++none+extra+", &command), Status::Ok);
    EXPECT_EQ(command.name(), "fault_probe");
    EXPECT_EQ(words(command), (std::vector<std::string_view>{"fault_probe", "none", "extra"}));
}

TEST(Autorun, NoWordsNamesNothing) {
    AutorunCommand command;
    EXPECT_EQ(AutorunCommand::parse("", &command), Status::InvArgs);
    EXPECT_EQ(AutorunCommand::parse("+++", &command), Status::InvArgs);
}

TEST(Autorun, TooManyWordsAreRefusedNotCut) {
    AutorunCommand command;
    ASSERT_EQ(AutorunCommand::parse("a+b+c+d+e+f+g+h", &command), Status::Ok);
    EXPECT_EQ(command.words().size(), AutorunCommand::MAX_WORDS);

    AutorunCommand untouched;
    ASSERT_EQ(AutorunCommand::parse("keep", &untouched), Status::Ok);
    EXPECT_EQ(AutorunCommand::parse("a+b+c+d+e+f+g+h+i", &untouched), Status::OutOfRange);
    EXPECT_EQ(words(untouched), (std::vector<std::string_view>{"keep"}));
}

// And at compile time, which is what constexpr is for here.
static_assert([] {
    AutorunCommand command;
    return AutorunCommand::parse("a++b", &command) == Status::Ok && command.words().size() == 2;
}());
} // namespace
