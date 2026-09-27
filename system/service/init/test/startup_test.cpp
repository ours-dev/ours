/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// init's startup list on the host: how config/init/startup splits into
/// lines, and what each line's command and capabilities come out as.
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <init/startup.hpp>

namespace {
using init::Capability;
using init::StartupEntry;
using init::StartupLines;
using ours::Status;

auto words(StartupEntry const &entry) -> std::vector<std::string_view> {
    return {entry.command().words().begin(), entry.command().words().end()};
}

auto lines(std::string_view const text) -> std::vector<std::string_view> {
    StartupLines list(text);
    std::vector<std::string_view> out;
    std::string_view line;
    while (list.next(&line)) {
        out.push_back(line);
    }
    return out;
}

TEST(StartupEntry, ACommandAloneAsksForNothing) {
    StartupEntry entry;
    ASSERT_EQ(StartupEntry::parse("shell", &entry), Status::Ok);
    EXPECT_EQ(entry.command().name(), "shell");
    EXPECT_TRUE(entry.capabilities().empty());
}

// The dev image's one line, as kernel_package() writes it.
TEST(StartupEntry, OneCapability) {
    StartupEntry entry;
    ASSERT_EQ(StartupEntry::parse("shell init", &entry), Status::Ok);
    EXPECT_EQ(words(entry), (std::vector<std::string_view>{"shell"}));
    EXPECT_TRUE(entry.capabilities().contains(Capability::Init));
    EXPECT_FALSE(entry.capabilities().contains(Capability::Bootfs));
    EXPECT_FALSE(entry.capabilities().contains(Capability::Job));
}

TEST(StartupEntry, ArgumentsAndEveryCapability) {
    StartupEntry entry;
    ASSERT_EQ(StartupEntry::parse("svc+a+b init,bootfs,job", &entry), Status::Ok);
    EXPECT_EQ(entry.command().name(), "svc");
    EXPECT_EQ(words(entry), (std::vector<std::string_view>{"svc", "a", "b"}));
    EXPECT_TRUE(entry.capabilities().contains(Capability::Init));
    EXPECT_TRUE(entry.capabilities().contains(Capability::Bootfs));
    EXPECT_TRUE(entry.capabilities().contains(Capability::Job));
}

TEST(StartupEntry, EmptyCapabilityWordsAreSkipped) {
    StartupEntry entry;
    ASSERT_EQ(StartupEntry::parse("svc ,job,,", &entry), Status::Ok);
    EXPECT_TRUE(entry.capabilities().contains(Capability::Job));
    EXPECT_FALSE(entry.capabilities().contains(Capability::Init));

    ASSERT_EQ(StartupEntry::parse("svc ", &entry), Status::Ok);
    EXPECT_TRUE(entry.capabilities().empty());
}

TEST(StartupEntry, UnknownCapabilityRefusesTheLine) {
    StartupEntry untouched;
    ASSERT_EQ(StartupEntry::parse("keep", &untouched), Status::Ok);
    EXPECT_EQ(StartupEntry::parse("svc init,network", &untouched), Status::NotSupported);
    // A second space lands inside a capability word.
    EXPECT_EQ(StartupEntry::parse("svc init job", &untouched), Status::NotSupported);
    EXPECT_EQ(untouched.command().name(), "keep");
}

TEST(StartupEntry, CommandErrorsAreAutorunsOwn) {
    StartupEntry entry;
    EXPECT_EQ(StartupEntry::parse(" init", &entry), Status::InvArgs);
    EXPECT_EQ(StartupEntry::parse("+++ init", &entry), Status::InvArgs);
    EXPECT_EQ(StartupEntry::parse("a+b+c+d+e+f+g+h+i init", &entry), Status::OutOfRange);
}

TEST(StartupLines, SplitsOnNewlinesAndSkipsEmptyLines) {
    EXPECT_EQ(lines("shell init\n"), (std::vector<std::string_view>{"shell init"}));
    EXPECT_EQ(lines("\na\n\nb c\n\n"), (std::vector<std::string_view>{"a", "b c"}));
    EXPECT_EQ(lines("no newline"), (std::vector<std::string_view>{"no newline"}));
    EXPECT_TRUE(lines("").empty());
    EXPECT_TRUE(lines("\n\n").empty());
}

// One bad line does not stop the lines after it: splitting never parses.
TEST(StartupLines, ABadLineDoesNotHideTheRest) {
    std::vector<std::string_view> good;
    StartupLines list("a init\nb nonsense\nc bootfs\n");
    std::string_view line;
    while (list.next(&line)) {
        StartupEntry entry;
        if (StartupEntry::parse(line, &entry) == Status::Ok) {
            good.push_back(entry.command().name());
        }
    }
    EXPECT_EQ(good, (std::vector<std::string_view>{"a", "c"}));
}

static_assert([] {
    StartupEntry entry;
    return StartupEntry::parse("shell init,job", &entry) == Status::Ok &&
           entry.capabilities().contains(Capability::Job);
}());
} // namespace
