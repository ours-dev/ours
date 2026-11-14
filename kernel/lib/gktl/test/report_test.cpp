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
    // One line per fault, each naming the command, the rule and the input
    // the way the console's bad-argument lines have always read.
    gktl::report_rejection("demo", {gktl::OptionFault::UnknownOption, "wat", "wat=1"});
    gktl::report_rejection("demo", {gktl::OptionFault::BadValue, "rounds", "1x"});
    gktl::report_rejection("demo", {gktl::OptionFault::BadChoice, "how", "nope"});
    gktl::report_rejection("demo", {gktl::OptionFault::MissingRequired, "mode", {}});
    gktl::report_rejection("demo", {gktl::OptionFault::TooManyWords, {}, {}});
    gktl::report_rejection("demo", {gktl::OptionFault::MalformedWord, {}, "=x"});
    SUCCEED();
}

TEST(GktlReport, UsageRendersFromTheDeclarations) {
    auto options = gktl::Options{"demo"};
    options.add<usize>("rounds").defaults_to(u64{64}).help("iterations per leg");
    options.add<bool>("csv");
    options.add<std::string_view>("how").choices(HOW_CHOICES).required();
    gktl::print_usage("demo", options.descriptors());
    SUCCEED();
}

} // namespace
