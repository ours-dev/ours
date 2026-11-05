/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Name matching for the `ktest` runner: one glob pattern, and the
// `:`-separated selection string a filter argument becomes. Kept free of
// kernel headers so the same code runs in the host tests.
#ifndef KTEST_FILTER_HPP
#define KTEST_FILTER_HPP 1

#include <string_view>

namespace ktest {

/// Whether |name| matches one glob |pattern|: `*` matches any run of
/// characters, `?` exactly one, and every other byte itself. Matching is
/// case-sensitive and covers the whole name.
[[nodiscard]]
constexpr auto matches_pattern(std::string_view const name, std::string_view const pattern)
    -> bool {
    auto name_at = usize{0};
    auto pattern_at = usize{0};
    auto star_at = std::string_view::npos;
    auto star_name_at = usize{0};

    while (name_at < name.size()) {
        if (pattern_at < pattern.size() &&
            (pattern[pattern_at] == '?' || pattern[pattern_at] == name[name_at])) {
            ++name_at;
            ++pattern_at;
            continue;
        }
        if (pattern_at < pattern.size() && pattern[pattern_at] == '*') {
            star_at = pattern_at;
            ++pattern_at;
            star_name_at = name_at;
            continue;
        }
        if (star_at == std::string_view::npos) {
            return false;
        }
        // Re-run the run of characters the last `*` can cover, one longer
        // each time, until one of them lets the rest of the pattern match.
        pattern_at = star_at + 1;
        ++star_name_at;
        name_at = star_name_at;
    }

    while (pattern_at < pattern.size() && pattern[pattern_at] == '*') {
        ++pattern_at;
    }
    return pattern_at == pattern.size();
}

/// Whether |name| is selected by |filter|: `:`-separated tokens, where a
/// token beginning with `-` excludes and every other token includes. With no
/// including token at all, everything not excluded matches.
[[nodiscard]]
constexpr auto matches_filter(std::string_view const name, std::string_view const filter) -> bool {
    if (filter.empty()) {
        return true;
    }

    auto has_inclusion = false;
    auto included = false;
    auto rest = filter;
    while (!rest.empty()) {
        auto const colon = rest.find(':');
        auto const token = rest.substr(0, colon);
        if (colon == std::string_view::npos) {
            rest = {};
        } else {
            rest = rest.substr(colon + 1);
        }
        if (token.empty()) {
            continue;
        }
        if (token.front() == '-') {
            if (matches_pattern(name, token.substr(1))) {
                return false;
            }
            continue;
        }
        has_inclusion = true;
        if (matches_pattern(name, token)) {
            included = true;
        }
    }
    return included || !has_inclusion;
}

} // namespace ktest

#endif // #ifndef KTEST_FILTER_HPP
