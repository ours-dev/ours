/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Run options: the `name=value` tokens the `ktest` command was given after its
// selection. They exist for the few tests whose behaviour needs a value per
// run -- a round count, a size -- and a test that reads one always supplies a
// fallback, so `ktest all` and a bare `ktest <filter>` still run the ordinary
// configuration.
#ifndef KTEST_OPTIONS_HPP
#define KTEST_OPTIONS_HPP 1

#include <string_view>

namespace ktest {

struct Option {
    std::string_view name;
    std::string_view value;
};

/// A run carries at most this many options: the console passes at most 16
/// arguments including the command and the selection, and every table here is
/// fixed-size.
inline constexpr usize MAX_RUN_OPTIONS = 14;

/// Parses one token: `name=value`, or `name` alone for a presence-only option
/// (an empty value). False when the name is empty; the shell has already
/// removed the spaces, so everything else is part of the token.
[[nodiscard]]
constexpr auto parse_option(std::string_view const token, Option &out) -> bool {
    auto const equals = token.find('=');
    auto const name = equals == std::string_view::npos ? token : token.substr(0, equals);
    if (name.empty()) {
        return false;
    }
    out.name = name;
    out.value = equals == std::string_view::npos ? std::string_view{} : token.substr(equals + 1);
    return true;
}

/// The first option named |name| among |entries|, or null.
[[nodiscard]]
constexpr auto find_option(Option const *const entries, usize const count,
                           std::string_view const name) -> Option const * {
    for (auto i = usize{0}; i < count; ++i) {
        if (entries[i].name == name) {
            return &entries[i];
        }
    }
    return nullptr;
}

/// Whether the running test's invocation was given |name|.
[[nodiscard]]
auto has_option(std::string_view name) -> bool;

/// |name|'s value, or |fallback| when the invocation did not carry it. The
/// value is part of the console line and is only valid while the test runs.
[[nodiscard]]
auto option(std::string_view name, std::string_view fallback = {}) -> std::string_view;

/// |name| parsed as an unsigned decimal, or |fallback| when the invocation did
/// not carry it. A value that is not a number is reported at error level and
/// |fallback| is used, so the run fails on the report rather than on a
/// silently different test.
[[nodiscard]]
auto option_usize(std::string_view name, usize fallback) -> usize;

} // namespace ktest

#endif // #ifndef KTEST_OPTIONS_HPP
