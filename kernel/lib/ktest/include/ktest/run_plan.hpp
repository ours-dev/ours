/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The runner's own options: shuffle, repeat and shard. They decide how one
// `ktest all` run walks the registry, so they sit beside the filter as pure
// string_view arithmetic, free of kernel headers, and the host tests exercise
// the same parsing and selection math the runner runs.
#ifndef KTEST_RUN_PLAN_HPP
#define KTEST_RUN_PLAN_HPP 1

#include <string_view>

namespace ktest {

/// The seed a bare `shuffle` runs with: fixed, so the default order is
/// reproducible from the SHUFFLE line alone. The fractional digits of pi in
/// hex; chosen to be arbitrary, and nothing hangs on its value.
inline constexpr u64 DEFAULT_SHUFFLE_SEED = 0x243F6A8885A308D3;

/// The registry size a shuffle can plan. The permutation lives in one fixed
/// array on the runner's side, so a registry beyond this bound declines to
/// shuffle instead of growing the array; 256 is far above the linked tests.
inline constexpr usize MAX_SHUFFLE_TESTS = 256;

/// One `ktest all` run's iteration plan. The defaults are the plain run: in
/// section order, once, every test.
struct RunPlan {
    bool shuffle{false};
    u64 seed{DEFAULT_SHUFFLE_SEED};
    usize repeat{1};
    usize shard_index{0};
    usize shard_count{1};
};

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

/// Parses an unsigned decimal: digits only, no sign, no separators. False for
/// an empty value, a stray character, or a value beyond usize's range.
[[nodiscard]]
constexpr auto parse_decimal(std::string_view const value, usize &out) -> bool {
    if (value.empty()) {
        return false;
    }
    auto parsed = usize{0};
    for (auto const c: value) {
        if (c < '0' || c > '9') {
            return false;
        }
        auto const digit = static_cast<usize>(c - '0');
        if (parsed > (~usize{0} - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    out = parsed;
    return true;
}

/// Parses a shuffle seed: `0x`- or `0X`-prefixed hexadecimal, or decimal, and
/// an empty value is the fixed default. False for anything else or a value
/// beyond u64's range.
[[nodiscard]]
constexpr auto parse_seed(std::string_view const value, u64 &out) -> bool {
    if (value.empty()) {
        out = DEFAULT_SHUFFLE_SEED;
        return true;
    }
    auto digits = value;
    auto base = u64{10};
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
        if (digits.empty()) {
            return false;
        }
    }
    auto parsed = u64{0};
    for (auto const c: digits) {
        auto digit = u64{0};
        if (c >= '0' && c <= '9') {
            digit = static_cast<u64>(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = static_cast<u64>(c - 'a') + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = static_cast<u64>(c - 'A') + 10;
        } else {
            return false;
        }
        if (parsed > (~u64{0} - digit) / base) {
            return false;
        }
        parsed = parsed * base + digit;
    }
    out = parsed;
    return true;
}

/// Splits `M/N` into its two unsigned decimals. Only the shape: the caller
/// checks the range rule with shard_valid(), so a malformed value and an
/// out-of-range one can carry their own messages.
[[nodiscard]]
constexpr auto parse_shard(std::string_view const value, usize &m, usize &n) -> bool {
    auto const slash = value.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    if (!parse_decimal(value.substr(0, slash), m)) {
        return false;
    }
    return parse_decimal(value.substr(slash + 1), n);
}

/// Whether |m| of |n| is a usable shard: n > 0 and m < n, so every registry
/// index falls in exactly one shard of the stride.
[[nodiscard]]
constexpr auto shard_valid(usize const m, usize const n) -> bool {
    return n != 0 && m < n;
}

/// One splitmix64 step: the state advances by the golden-ratio constant, and
/// the reached value is mixed forward. Chosen because it is a handful of
/// arithmetic operations, seedable from one word, and its low bits are as
/// mixed as its high ones, which the modulo in shuffle_indices needs.
[[nodiscard]]
constexpr auto next_random(u64 &state) -> u64 {
    state += 0x9E3779B97F4A7C15;
    auto mixed = state;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EB;
    return mixed ^ (mixed >> 31);
}

/// Fills |indices[0..count)| with the registry indices in shuffled order:
/// identity first, then Fisher-Yates, drawing one position per step from
/// |seed|, so the same seed always gives the same order. The draw's modulo
/// leaves a bias below one part in 2^64/count, which no test order needs to
/// be free of.
constexpr auto shuffle_indices(usize *const indices, usize const count, u64 const seed) -> void {
    for (auto i = usize{0}; i < count; ++i) {
        indices[i] = i;
    }
    auto state = seed;
    for (auto i = count; i > 1; --i) {
        auto const j = static_cast<usize>(next_random(state) % static_cast<u64>(i));
        auto const held = indices[i - 1];
        indices[i - 1] = indices[j];
        indices[j] = held;
    }
}

/// Whether the registry index |index| belongs to shard |m| of |n|: 0-based,
/// strided over the registry's section order. |n| must have passed
/// shard_valid(); with n == 1 every index belongs.
[[nodiscard]]
constexpr auto in_shard(usize const index, usize const m, usize const n) -> bool {
    return index % n == m;
}

} // namespace ktest

#endif // #ifndef KTEST_RUN_PLAN_HPP
