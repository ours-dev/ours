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

#include <string_view>

#include <logz4/log.hpp>

namespace gktl {

/// The one error line a rejection prints, the shape every command's
/// bad-argument line carries: `<cmd>: ERROR <the rule> '<the input>'`. A
/// command handler calls this and returns 1; it never writes its own usage
/// line by hand any more, because the declarations below the rejection
/// already know the whole grammar. The command name comes from the
/// declarations too: a handler that renames its Options cannot leave its
/// error lines calling it by the old name.
inline auto report_rejection(Options const &options, Rejection const &rejection) -> void {
    auto const cmd = options.cmd();
    switch (rejection.fault) {
        case OptionFault::TooManyWords:
            log::error("{}: ERROR {}", cmd, rejection_message(rejection.fault));
            return;
        case OptionFault::MalformedWord:
            log::error("{}: ERROR {} '{}'", cmd, rejection_message(rejection.fault),
                       rejection.word);
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
inline auto print_usage(Options const &options) -> void {
    auto const cmd = options.cmd();
    log::warn("usage: {}", cmd);
    for (auto const &descriptor: options.descriptors()) {
        char buffer[112];
        auto const word = usage_word(descriptor, buffer, sizeof(buffer));
        auto help = std::string_view{};
        if (descriptor.help != nullptr) {
            help = std::string_view{descriptor.help};
        }
        auto separator = std::string_view{" "};
        if (!help.empty()) {
            separator = " - ";
        }
        if (!descriptor.has_default) {
            if (help.empty()) {
                log::warn("  {}", word);
            } else {
                log::warn("  {} - {}", word, help);
            }
            continue;
        }
        // The default's text is the shape's to render -- digits, true/false,
        // or the quoted word -- so this loop stays shape-blind.
        char default_buffer[32];
        auto default_at = usize{0};
        internal::shape_row(descriptor.kind)
            .default_text(descriptor.default_value, default_buffer, sizeof(default_buffer),
                          default_at);
        auto const default_text = std::string_view{default_buffer, default_at};
        log::warn("  {}{}{} (default {})", word, separator, help, default_text);
    }
}

/// What a handler does with a rejection: the pinned error line and the
/// generated usage, in that order.
inline auto reject(Options const &options, Rejection const &rejection) -> void {
    report_rejection(options, rejection);
    print_usage(options);
}

} // namespace gktl

#endif // #ifndef GKTL_REPORT_HPP
