/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The reporting half of declared options: the pinned error line a rejection
// prints and the usage the declarations generate. The parsing half
// (<gktl/options.hpp>) is pure string arithmetic so the host can test it;
// this half prints, so it needs the log and only the kernel includes it.
#ifndef GKTL_REPORT_HPP
#define GKTL_REPORT_HPP 1

#include <gktl/options.hpp>

#include <span>
#include <string_view>

#include <logz4/log.hpp>

namespace gktl {

/// The one error line a rejection prints, the shape every command's
/// bad-argument line carries: `<cmd>: ERROR <the rule> '<the input>'`. A
/// command handler calls this and returns 1; it never writes its own usage
/// line by hand any more, because the declarations below the rejection
/// already know the whole grammar.
inline auto report_rejection(std::string_view const cmd, Rejection const &rejection) -> void {
    switch (rejection.fault) {
        case OptionFault::TooManyWords:
        case OptionFault::MalformedWord:
            log::error("{}: ERROR {}", cmd, rejection_message(rejection.fault));
            return;
        case OptionFault::MissingRequired:
            log::error("{}: ERROR {} '{}'", cmd, rejection_message(rejection.fault),
                       rejection.name);
            return;
        case OptionFault::UnknownOption:
        case OptionFault::BadValue:
        case OptionFault::BadChoice:
            log::error("{}: ERROR {} '{}' (given '{}')", cmd, rejection_message(rejection.fault),
                       rejection.name, rejection.word);
            return;
    }
}

/// The usage the declarations generate: one line naming the command, then one
/// line per option -- the fragment (`how=<panic|panic-locked>`,
/// `[rounds=N]`, `[csv]`), the declaration's help, and its default when it
/// declares one. This is what replaces the hand-written usage strings each
/// command used to duplicate beside its help text.
inline auto print_usage(std::string_view const cmd,
                        std::span<OptionDescriptor const> const descriptors) -> void {
    log::warn("usage: {}", cmd);
    for (auto const &descriptor: descriptors) {
        char buffer[112];
        auto const word = usage_word(descriptor, buffer, sizeof(buffer));
        auto const help =
            descriptor.help != nullptr ? std::string_view{descriptor.help} : std::string_view{};
        auto const separator = help.empty() ? " " : " - ";
        if (!descriptor.has_default) {
            if (help.empty()) {
                log::warn("  {}", word);
            } else {
                log::warn("  {} - {}", word, help);
            }
            continue;
        }
        switch (descriptor.kind) {
            case OptionKind::U64:
                log::warn("  {}{}{} (default {})", word, separator, help, descriptor.default_.i);
                break;
            case OptionKind::Bool:
                log::warn("  {}{}{} (default {})", word, separator, help, descriptor.default_.b);
                break;
            case OptionKind::Str:
                log::warn("  {}{}{} (default '{}')", word, separator, help, descriptor.default_.s);
                break;
        }
    }
}

/// What a handler does with a rejection: the pinned error line and the
/// generated usage, in that order. The command name is repeated because the
/// handler has both at hand and the pair travels together everywhere.
inline auto reject(std::string_view const cmd, Options const &options, Rejection const &rejection)
    -> void {
    report_rejection(cmd, rejection);
    print_usage(cmd, options.descriptors());
}

} // namespace gktl

#endif // #ifndef GKTL_REPORT_HPP
