/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Host-side tests for the options' reporting half: the pinned error lines a
// rejection maps to and the usage the declarations generate. The log sink on
// the host writes where its kernel counterpart writes on the target, so the
// lines these cases drive are the ones a console session sees.
#include <gktl/report.hpp>

#include <gtest/gtest.h>

// The host's stand-in for where a failed assertion ends -- defined once for
// this binary, beside the suite that first needs it (options_test.cpp).

namespace {

constexpr std::string_view HOW_CHOICES[] = {"panic", "panic-locked", "panic-smp"};

TEST(GktlReport, EveryFaultPrintsItsPinnedLine) {
    auto options = gktl::Options{"demo"};
    options.add<u64>("rounds").defaults_to(u64{64});
    options.add<std::string_view>("how").choices(HOW_CHOICES);
    options.add<std::string_view>("mode").defaults_to("time").required();
    // One line per fault, each naming the command, the rule and the input
    // the way the console's bad-argument lines have always read. The command
    // name comes from the declarations, not from the caller.
    gktl::report_rejection(options, {gktl::OptionFault::UnknownOption, "wat", "wat=1"});
    gktl::report_rejection(options, {gktl::OptionFault::BadValue, "rounds", "1x"});
    gktl::report_rejection(options, {gktl::OptionFault::BadChoice, "how", "nope"});
    gktl::report_rejection(options, {gktl::OptionFault::MissingRequired, "mode", {}});
    gktl::report_rejection(options, {gktl::OptionFault::TooManyWords, {}, {}});
    gktl::report_rejection(options, {gktl::OptionFault::MalformedWord, {}, "=x"});
    SUCCEED();
}

TEST(GktlReport, UsageRendersFromTheDeclarations) {
    auto options = gktl::Options{"demo"};
    options.add<usize>("rounds").defaults_to(u64{64}).help("iterations per leg");
    options.add<bool>("csv");
    options.add<std::string_view>("how").choices(HOW_CHOICES).required();
    gktl::print_usage(options);
    SUCCEED();
}

} // namespace
