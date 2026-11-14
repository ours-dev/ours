/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The runner's own options: shuffle, repeat and shard. They decide how one
// `ktest all` run walks the registry, so they sit beside the filter as pure
// string arithmetic, free of kernel headers, and the host tests exercise the
// same parsing and selection math the runner runs. A run's words build a
// RunPlan once; the runner walks the plan, and the tests read the words the
// runner did not act on.
#ifndef KTEST_RUN_PLAN_HPP
#define KTEST_RUN_PLAN_HPP 1

#include <ktest/options.hpp>

#include <span>
#include <string_view>

#include <gktl/options.hpp>
#include <ours/status.hpp>
#include <ustl/result.hpp>

namespace ktest {

/// The seed a bare `shuffle` runs with: fixed, so the default order is
/// reproducible from the SHUFFLE line alone. The fractional digits of pi in
/// hex; chosen to be arbitrary, and nothing hangs on its value.
inline constexpr u64 DEFAULT_SHUFFLE_SEED = 0x243F6A8885A308D3;

/// The registry size a shuffle can plan. The permutation lives in one fixed
/// array on the runner's side, so a registry beyond this bound declines to
/// shuffle instead of growing the array; 256 is far above the linked tests.
inline constexpr usize MAX_SHUFFLE_TESTS = 256;

/// Which of the runner's own options |name| is. Anything else is an option
/// for the tests to read, and the runner ignores it.
enum class RunnerOption {
    /// `shuffle` or `shuffle=SEED`: run the selection in a seeded shuffled
    /// order instead of section order.

    Shuffle,

    /// `repeat=N`: run the whole selection N times; the SUMMARY counts every
    /// execution.
    Repeat,

    /// `shard=M/N`: run only the tests whose registry index i satisfies
    /// i % N == M.
    Shard,

    /// Any other name.
    Other,
};

[[nodiscard]]
constexpr auto runner_option(std::string_view const name) -> RunnerOption {
    if (name == "shuffle") {
        return RunnerOption::Shuffle;
    }
    if (name == "repeat") {
        return RunnerOption::Repeat;
    }
    if (name == "shard") {
        return RunnerOption::Shard;
    }
    return RunnerOption::Other;
}

/// Parses an unsigned decimal or hexadecimal digit run: digits only, no sign,
/// no separators, |base| 10 or 16. InvArgs for an empty value, a
/// character that is not a digit of |base|, or a value beyond u64's range.
/// The one digit primitive every number-shaped word here goes through; the
/// body is gktl's, promoted from here when the commands' options builder
/// needed the same primitive and the runner keeps the name for its callers.
[[nodiscard]]
constexpr auto parse_u64(std::string_view const digits, u64 const base = 10)
    -> ustl::Result<u64, ours::Status> {
    return gktl::parse_u64(digits, base);
}

/// Parses a shuffle seed: `0x`- or `0X`-prefixed hexadecimal, or decimal, and
/// an empty value is the fixed default. InvArgs for anything else or a
/// value beyond u64's range.
[[nodiscard]]
constexpr auto parse_seed(std::string_view const value) -> ustl::Result<u64, ours::Status> {
    if (value.empty()) {
        return ustl::ok(DEFAULT_SHUFFLE_SEED);
    }
    auto digits = value;
    auto base = u64{10};
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
        if (digits.empty()) {
            return ustl::err(ours::Status::InvArgs);
        }
    }
    return parse_u64(digits, base);
}

/// One shard of a sharded run: the M and the N of `shard=M/N`. Every registry
/// index belongs to exactly one shard of the stride, which the M < N rule
/// keeps.
struct Shard {
    usize m{0};
    usize n{1};

    /// Parses `M/N`: both halves unsigned decimals, and the range rule M < N
    /// with N > 0 checked here, so one parse answers both shapes of bad
    /// value — a malformed one and an out-of-range one — with the same
    /// InvArgs.
    [[nodiscard]]
    static constexpr auto parse(std::string_view const value) -> ustl::Result<Shard, ours::Status> {
        auto const slash = value.find('/');
        if (slash == std::string_view::npos) {
            return ustl::err(ours::Status::InvArgs);
        }
        auto const m = parse_u64(value.substr(0, slash));
        if (m.is_err()) {
            return ustl::err(m.unwrap_err());
        }
        auto const n = parse_u64(value.substr(slash + 1));
        if (n.is_err()) {
            return ustl::err(n.unwrap_err());
        }
        auto parsed = Shard{static_cast<usize>(m.unwrap()), static_cast<usize>(n.unwrap())};
        if (parsed.n == 0 || parsed.m >= parsed.n) {
            return ustl::err(ours::Status::InvArgs);
        }
        return ustl::ok(parsed);
    }
};

/// Which rule a run's words broke.
enum class PlanFault {
    /// More words than one run records — beyond MAX_RUN_OPTIONS.
    TooManyWords,

    /// A word that is not a `name=value` token: an empty name.
    MalformedWord,

    /// shuffle, repeat or shard on a run that is not `ktest all`; the
    /// runner's own options belong to the whole-registry run alone.
    RunnerWordOnSelection,

    /// shuffle's seed is not a number.
    BadSeed,

    /// repeat is not a count of at least 1.
    BadRepeat,

    /// shard is not M/N with 0 <= M < N.
    BadShard,
};

/// A run whose words were refused: which rule broke and the word that broke
/// it — the option's value, the text the console line quotes back. Empty
/// where the line names no word.
struct PlanRejection {
    PlanFault fault;
    std::string_view word{};
};

/// One `ktest all` run's iteration plan. The defaults are the plain run: in
/// section order, once, every test. Built once from the run's words; the
/// runner walks it, and the tests read the words through it.
struct RunPlan {
    bool shuffle{false};
    u64 seed{DEFAULT_SHUFFLE_SEED};
    usize repeat{1};
    Shard shard{};

    /// Every word the run carried, in the order given: the ones the runner
    /// acted on set the fields above on the way through, and all of them sit
    /// here for the runner to log and the tests to read.
    Option options[MAX_RUN_OPTIONS]{};
    usize option_count{};

    /// Builds the plan from a run's own words. Each word is a `name=value`
    /// token; the runner's own — shuffle, repeat, shard — set the fields and
    /// are validated on the way through, and every word is recorded in the
    /// order given. |runner_options_allowed| is false for a run that selected
    /// a filter, where the runner's own options do not belong. The rejection
    /// names the first rule the words broke, in command order.
    [[nodiscard]]
    static constexpr auto from_words(std::span<std::string_view const> words,
                                     bool const runner_options_allowed)
        -> ustl::Result<RunPlan, PlanRejection> {
        auto plan = RunPlan{};
        if (words.size() > MAX_RUN_OPTIONS) {
            return ustl::err(PlanRejection{PlanFault::TooManyWords, {}});
        }
        for (auto const word: words) {
            auto parsed = Option{};
            if (!parse_option(word, parsed)) {
                return ustl::err(PlanRejection{PlanFault::MalformedWord, word});
            }
            plan.options[plan.option_count] = parsed;
            plan.option_count += 1;
        }
        for (auto i = usize{0}; i < plan.option_count; ++i) {
            auto const kind = runner_option(plan.options[i].name);
            if (kind != RunnerOption::Other && !runner_options_allowed) {
                return ustl::err(PlanRejection{PlanFault::RunnerWordOnSelection, {}});
            }
            auto const &option = plan.options[i];
            switch (kind) {
                case RunnerOption::Shuffle: {
                    auto const seed = parse_seed(option.value);
                    if (seed.is_err()) {
                        return ustl::err(PlanRejection{PlanFault::BadSeed, option.value});
                    }
                    plan.shuffle = true;
                    plan.seed = seed.unwrap();
                    break;
                }
                case RunnerOption::Repeat: {
                    auto const passes = parse_u64(option.value);
                    if (passes.is_err() || passes.unwrap() == 0) {
                        return ustl::err(PlanRejection{PlanFault::BadRepeat, option.value});
                    }
                    plan.repeat = static_cast<usize>(passes.unwrap());
                    break;
                }
                case RunnerOption::Shard: {
                    auto const shard = Shard::parse(option.value);
                    if (shard.is_err()) {
                        return ustl::err(PlanRejection{PlanFault::BadShard, option.value});
                    }
                    plan.shard = shard.unwrap();
                    break;
                }
                case RunnerOption::Other:
                    break;
            }
        }
        return ustl::ok(plan);
    }
};

/// One splitmix64 step: the state advances by the golden-ratio constant, and
/// the reached value is mixed forward. Chosen because it is a handful of
/// arithmetic operations, seedable from one word, and its low bits are as
/// mixed as its high ones, which the modulo in shuffle() needs.
[[nodiscard]]
constexpr auto next_random(u64 &state) -> u64 {
    state += 0x9E3779B97F4A7C15;
    auto mixed = state;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EB;
    return mixed ^ (mixed >> 31);
}

/// Fills |indices| with the registry indices in shuffled order: identity
/// first, then Fisher-Yates over the span, drawing one position per step from
/// |seed|, so the same seed always gives the same order. The draw's modulo
/// leaves a bias below one part in 2^64/count, which no test order needs to
/// be free of.
constexpr auto shuffle(std::span<usize> indices, u64 const seed) -> void {
    for (auto i = usize{0}; i < indices.size(); ++i) {
        indices[i] = i;
    }
    auto state = seed;
    for (auto i = indices.size(); i > 1; --i) {
        auto const j = static_cast<usize>(next_random(state) % static_cast<u64>(i));
        auto const held = indices[i - 1];
        indices[i - 1] = indices[j];
        indices[j] = held;
    }
}

/// Whether the registry index |index| belongs to shard |m| of |n|: 0-based,
/// strided over the registry's section order. |n| must have come out of a
/// Shard::parse; with n == 1 every index belongs.
[[nodiscard]]
constexpr auto in_shard(usize const index, usize const m, usize const n) -> bool {
    return index % n == m;
}

} // namespace ktest

#endif // #ifndef KTEST_RUN_PLAN_HPP
