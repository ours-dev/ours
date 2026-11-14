/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Host-side tests for the declared-options builder: the word grammar, the
// typed parses, every rejection fault, the pass-through, the last-wins rule,
// and the usage fragments. The kernel command that runs these shapes is the
// same Options::parse a host case drives -- the header is free of kernel
// headers exactly so this suite can compile it.
#include <gktl/options.hpp>

#include <string_view>

#include <gtest/gtest.h>

// The assertion the header's misuse checks go through ends in ours::vpanic;
// the host stands in with an abort, the way the kernel's own host suites do.
namespace ours {
auto vpanic(std::string_view) -> void {
    std::abort();
}
} // namespace ours

namespace {

// A `0x`-prefixed or decimal hook, the shape a shuffle seed parses with.
constexpr auto parse_seed(std::string_view const value) -> ustl::Result<u64, ours::Status> {
    if (value.empty()) {
        return ustl::ok(u64{0});
    }
    auto digits = value;
    auto base = u64{10};
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
    }
    return gktl::parse_u64(digits, base);
}

// The choice table a command owns with static storage -- the contract
// Arg::choices states: a brace list's backing array would die with the
// statement, and the acceptance check would read it dead.
constexpr std::string_view HOW_CHOICES[] = {"panic", "panic-locked", "panic-smp"};

// The declarations most shape cases share: a number with a default, a
// boolean, a string with choices, and a required string with a default.
// Options is used in place -- its handles point into its own table -- so a
// test builds one on its frame and declares into it through this helper.
auto declare_demo(gktl::Options &options) -> void {
    options.add<usize>("rounds").defaults_to(u64{64}).help("iterations per leg");
    options.add<bool>("csv").defaults_to(false);
    options.add<std::string_view>("how").choices(HOW_CHOICES);
    options.add<std::string_view>("mode").defaults_to("time");
}

TEST(GktlWord, SplitsOnTheFirstEquals) {
    auto word = gktl::Word{};
    ASSERT_TRUE(gktl::parse_word("rounds=64", word));
    EXPECT_EQ(word.name, "rounds");
    EXPECT_EQ(word.value, "64");
    EXPECT_TRUE(word.has_value);

    ASSERT_TRUE(gktl::parse_word("csv", word));
    EXPECT_EQ(word.name, "csv");
    EXPECT_EQ(word.value, "");
    EXPECT_FALSE(word.has_value);

    ASSERT_TRUE(gktl::parse_word("mode=", word));
    EXPECT_EQ(word.name, "mode");
    EXPECT_EQ(word.value, "");
    EXPECT_TRUE(word.has_value);

    EXPECT_FALSE(gktl::parse_word("=value", word));
    EXPECT_FALSE(gktl::parse_word("=", word));
    EXPECT_FALSE(gktl::parse_word("", word));
}

TEST(GktlPrimitives, ParseU64TakesDigitsOnly) {
    EXPECT_EQ(gktl::parse_u64("64").unwrap(), u64{64});
    EXPECT_EQ(gktl::parse_u64("0").unwrap(), u64{0});
    EXPECT_EQ(gktl::parse_u64("ff", 16).unwrap(), u64{255});
    EXPECT_EQ(gktl::parse_u64("FF", 16).unwrap(), u64{255});
    EXPECT_TRUE(gktl::parse_u64("").is_err());
    EXPECT_TRUE(gktl::parse_u64("1x").is_err());
    EXPECT_TRUE(gktl::parse_u64("-1").is_err());
    EXPECT_TRUE(gktl::parse_u64(" 1").is_err());
    EXPECT_TRUE(gktl::parse_u64("18446744073709551616").is_err());
    EXPECT_EQ(gktl::parse_u64("18446744073709551615").unwrap(), ~u64{0});
}

TEST(GktlPrimitives, ParseBoolStrictWordsOnly) {
    EXPECT_TRUE(gktl::parse_bool_strict("1").value());
    EXPECT_TRUE(gktl::parse_bool_strict("true").value());
    EXPECT_TRUE(gktl::parse_bool_strict("yes").value());
    EXPECT_FALSE(gktl::parse_bool_strict("0").value());
    EXPECT_FALSE(gktl::parse_bool_strict("false").value());
    EXPECT_FALSE(gktl::parse_bool_strict("no").value());
    EXPECT_TRUE(gktl::parse_bool_strict("flase").is_none());
    EXPECT_TRUE(gktl::parse_bool_strict("").is_none());
    EXPECT_TRUE(gktl::parse_bool_strict("on").is_none());
}

TEST(GktlParse, BareBooleanIsTrue) {
    auto options = gktl::Options{"demo"};
    declare_demo(options);
    // The demo's `mode` is required, so the line carries it beside the bare
    // boolean; the run itself would stop with MissingRequired without it.
    auto const parsed = options.parse({"csv", "mode=time"});
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_TRUE(parsed.unwrap().get<bool>("csv"));
}

TEST(GktlParse, BooleanTakesTheStrictWords) {
    auto options = gktl::Options{"demo"};
    declare_demo(options);
    EXPECT_EQ(options.parse({"csv=true"}).unwrap().get<bool>("csv"), true);
    EXPECT_EQ(options.parse({"csv=no"}).unwrap().get<bool>("csv"), false);
    auto const rejected = options.parse({"csv=flase"});
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(rejected.unwrap_err().fault, gktl::OptionFault::BadValue);
    EXPECT_EQ(rejected.unwrap_err().name, "csv");
    EXPECT_EQ(rejected.unwrap_err().word, "flase");
}

TEST(GktlParse, NumberTakesDigitsAndDefaultsOnBareWord) {
    auto options = gktl::Options{"demo"};
    declare_demo(options);
    EXPECT_EQ(options.parse({"rounds=128"}).unwrap().get<usize>("rounds"), usize{128});
    EXPECT_EQ(options.parse({"rounds"}).unwrap().get<u64>("rounds"), u64{64});
    // `rounds=` is an empty value, not a missing one, and empty is not digits.
    EXPECT_EQ(options.parse({"rounds="}).unwrap_err().fault, gktl::OptionFault::BadValue);
    EXPECT_EQ(options.parse({"rounds=1x"}).unwrap_err().fault, gktl::OptionFault::BadValue);
}

TEST(GktlParse, BareNumberWithoutDefaultIsABadValue) {
    auto options = gktl::Options{"demo"};
    options.add<u64>("count");
    auto const rejected = options.parse({"count"});
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(rejected.unwrap_err().fault, gktl::OptionFault::BadValue);
}

TEST(GktlParse, StringTakesChoicesAndDefaults) {
    auto options = gktl::Options{"demo"};
    declare_demo(options);
    EXPECT_EQ(options.parse({"how=panic"}).unwrap().get<std::string_view>("how"), "panic");
    auto const rejected = options.parse({"how=nope"});
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(rejected.unwrap_err().fault, gktl::OptionFault::BadChoice);
    EXPECT_EQ(rejected.unwrap_err().name, "how");
    EXPECT_EQ(rejected.unwrap_err().word, "nope");
    // A bare string with a default means the default; without one, nothing.
    EXPECT_EQ(options.parse({"mode=time"}).unwrap().get<std::string_view>("mode"), "time");
}

TEST(GktlParse, UnknownWordIsRejectedNamingItself) {
    auto options = gktl::Options{"demo"};
    declare_demo(options);
    auto const rejected = options.parse({"mode=time", "wat=1"});
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(rejected.unwrap_err().fault, gktl::OptionFault::UnknownOption);
    EXPECT_EQ(rejected.unwrap_err().name, "wat");
    EXPECT_EQ(rejected.unwrap_err().word, "wat=1");
}

TEST(GktlParse, PassThroughKeepsTheUnclaimedWords) {
    auto options = gktl::Options{"demo"};
    options.add<usize>("rounds").defaults_to(u64{64});
    options.pass_through();
    auto const parsed = options.parse({"seed=7", "rounds=2", "plain"});
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.unwrap().get<usize>("rounds"), usize{2});
    auto const passed = parsed.unwrap().passthrough();
    ASSERT_EQ(passed.size(), usize{2});
    EXPECT_EQ(passed[0].name, "seed");
    EXPECT_EQ(passed[0].value, "7");
    EXPECT_EQ(passed[1].name, "plain");
    EXPECT_FALSE(passed[1].has_value);
}

TEST(GktlParse, TheLastOccurrenceOfANameWins) {
    auto options = gktl::Options{"demo"};
    declare_demo(options);
    auto const parsed = options.parse({"rounds=1", "rounds=2", "rounds=3"});
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.unwrap().get<usize>("rounds"), usize{3});
    EXPECT_EQ(
        options.parse({"how=panic", "how=panic-locked"}).unwrap().get<std::string_view>("how"),
        "panic-locked");
}

TEST(GktlParse, MissingRequiredStopsTheRun) {
    auto options = gktl::Options{"demo"};
    options.add<u64>("count").required();
    auto const rejected = options.parse({});
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(rejected.unwrap_err().fault, gktl::OptionFault::MissingRequired);
    EXPECT_EQ(rejected.unwrap_err().name, "count");
    EXPECT_TRUE(rejected.unwrap_err().word.empty());
}

TEST(GktlParse, MalformedAndOverlongLinesAreNamed) {
    auto options = gktl::Options{"demo"};
    declare_demo(options);
    EXPECT_EQ(options.parse({"=x"}).unwrap_err().fault, gktl::OptionFault::MalformedWord);
    auto words = std::array<std::string_view, 17>{};
    for (auto &word: words) {
        word = "csv";
    }
    EXPECT_EQ(options.parse(words).unwrap_err().fault, gktl::OptionFault::TooManyWords);
}

TEST(GktlParse, CustomHookParsesTheShapesPlainDigitsCannot) {
    auto options = gktl::Options{"demo"};
    options.add<u64>("seed").defaults_to(u64{1}).parse(&parse_seed);
    EXPECT_EQ(options.parse({"seed=0x10"}).unwrap().get<u64>("seed"), u64{16});
    EXPECT_EQ(options.parse({"seed=10"}).unwrap().get<u64>("seed"), u64{10});
    EXPECT_EQ(options.parse({"seed=0x"}).unwrap_err().fault, gktl::OptionFault::BadValue);
}

TEST(GktlParse, TheConsoleShapeSkipsTheCommandWord) {
    auto options = gktl::Options{"demo"};
    declare_demo(options);
    // Three words the way the console's dispatch leaves them: each one
    // NUL-terminated in place, argv[0] the command itself.
    char cmd[] = "demo";
    char first[] = "mode=time";
    char second[] = "rounds=8";
    char *argv[] = {cmd, first, second, nullptr};
    auto const parsed = options.parse(3, argv);
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.unwrap().get<std::string_view>("mode"), "time");
    EXPECT_EQ(parsed.unwrap().get<usize>("rounds"), usize{8});
}

TEST(GktlParsed, ReadsZeroForAnOptionGivenNeither) {
    auto options = gktl::Options{"demo"};
    options.add<u64>("count");
    auto const parsed = options.parse({});
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.unwrap().get<u64>("count"), u64{0});
    EXPECT_FALSE(parsed.unwrap().has("count"));
}

TEST(GktlParsed, PassthroughIsEmptyWithoutTheFlag) {
    auto options = gktl::Options{"demo"};
    declare_demo(options);
    auto const parsed = options.parse({"mode=time"});
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_TRUE(parsed.unwrap().passthrough().empty());
}

TEST(GktlUsage, FragmentsRenderTheDeclaration) {
    auto options = gktl::Options{"demo"};
    declare_demo(options);
    char buffer[112];
    auto const descriptors = options.descriptors();
    ASSERT_EQ(descriptors.size(), usize{4});
    EXPECT_EQ(gktl::usage_word(descriptors[0], buffer, sizeof(buffer)), "[rounds=N]");
    EXPECT_EQ(gktl::usage_word(descriptors[1], buffer, sizeof(buffer)), "[csv]");
    EXPECT_EQ(gktl::usage_word(descriptors[2], buffer, sizeof(buffer)),
              "[how=<panic|panic-locked|panic-smp>]");
    EXPECT_EQ(gktl::usage_word(descriptors[3], buffer, sizeof(buffer)), "[mode=<text>]");

    auto required = gktl::Options{"crash"};
    constexpr std::string_view PANIC_ONLY[] = {"panic"};
    required.add<std::string_view>("how").required().choices(PANIC_ONLY);
    EXPECT_EQ(gktl::usage_word(required.descriptors()[0], buffer, sizeof(buffer)), "how=<panic>");
}

TEST(GktlUsage, AFragmantTooLongForTheBufferTruncates) {
    auto options = gktl::Options{"demo"};
    options.add<std::string_view>("mode").required();
    char tiny[8];
    auto const word = gktl::usage_word(options.descriptors()[0], tiny, sizeof(tiny));
    EXPECT_EQ(word, "mode=<te");
}

TEST(GktlRejection, EveryFaultHasItsFixedPhrase) {
    EXPECT_EQ(gktl::rejection_message(gktl::OptionFault::UnknownOption), "has no option named");
    EXPECT_EQ(gktl::rejection_message(gktl::OptionFault::BadValue), "could not read a value for");
    EXPECT_EQ(gktl::rejection_message(gktl::OptionFault::BadChoice),
              "is not one of the choices for");
    EXPECT_EQ(gktl::rejection_message(gktl::OptionFault::MissingRequired), "needs");
    EXPECT_EQ(gktl::rejection_message(gktl::OptionFault::TooManyWords),
              "carries more words than a command line can hold");
    EXPECT_EQ(gktl::rejection_message(gktl::OptionFault::MalformedWord),
              "carries a word with an empty name");
}

} // namespace
