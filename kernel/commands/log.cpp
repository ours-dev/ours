/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `log`: dump logz4's record ring, list the log domains, or move a level
// floor -- the global one or a single domain's. The logging library itself
// stays free of any dependency on gktl's command registry -- phys links
// //system/lib/logz4 and has no console at all.
#include <format>
#include <string_view>

#include <gktl/command.hpp>
#include <gktl/options.hpp>
#include <gktl/report.hpp>
#include <logz4/domain.hpp>
#include <logz4/filter.hpp>
#include <logz4/level.hpp>
#include <logz4/log.hpp>
#include <ustl/fmt/format_to.hpp>

namespace {
/// A reply to whoever typed the command, written straight to the console
/// sink as one line.
///
/// Not a log line: it would pass through the very floors this command sets,
/// so `log level error` followed by `log domains` would print nothing, and
/// a typo'd level would be reported at a severity the operator may just have
/// silenced. It is also kept out of the record ring, which holds what the
/// kernel said, not the console's answers -- the same routing dump_recent()
/// already uses for the lines it replays.
///
/// A reply longer than the buffer is cut rather than wrapped; one byte is
/// held back so the newline always fits.
template <typename... Args>
auto reply(std::format_string<Args const &...> fmt, Args const &...args) -> void {
    char line[128];
    auto const result = ustl::fmt::format_to_n(line, sizeof(line) - 1, fmt, args...);
    auto length = static_cast<usize>(result.out - line);
    line[length++] = '\n';
    logz4::get_global_logger().write(line, length);
}

/// `default` hands a domain back to the global floor. A word rather than a
/// level name because it is not a level: the domain stops having one.
constexpr std::string_view FOLLOW_GLOBAL = "default";

/// The verbs `log verb=` names, the static table the choices check reads.
constexpr std::string_view VERBS[] = {"dump", "domains", "level"};

auto find_domain(std::string_view const name) -> logz4::Domain * {
    auto *const domain = logz4::Domain::find(name);
    if (domain == nullptr) {
        reply("log: no domain '{}' (see `log verb=domains`)", name);
    }
    return domain;
}

/// `log verb=dump [domain=<d>]`
auto dump(gktl::Parsed const &values) -> int {
    if (!values.has("domain")) {
        logz4::dump_recent();
        return 0;
    }
    auto *const domain = find_domain(values.get<std::string_view>("domain"));
    if (domain == nullptr) {
        return 1;
    }
    logz4::dump_recent(*domain);
    return 0;
}

/// `log domains`: every domain in the image with the floor it filters at.
auto list_domains() -> int {
    reply("log: global floor {}", logz4::level_name(logz4::min_level()));
    for (auto const &domain: logz4::Domain::all()) {
        if (auto const own = domain.own_min_level(); own.has_value()) {
            reply("  {:<12} {}", domain.name(), logz4::level_name(*own));
        } else {
            reply("  {:<12} {} ({})", domain.name(), logz4::level_name(domain.min_level()),
                  FOLLOW_GLOBAL);
        }
    }
    return 0;
}

/// `log verb=level level=<l>` moves the global floor; `log verb=level
/// domain=<d> level=<l|default>` gives one domain its own floor, or takes it
/// away. Which of the two runs is the domain word's presence, so a domain
/// that happens to be called "warn" is still reachable -- the rule the
/// argument count used to settle, now settled by which options the line
/// carried.
auto set_level(gktl::Parsed const &values) -> int {
    auto const word = values.get<std::string_view>("level");
    if (!values.has("domain")) {
        auto level = logz4::Level::Info;
        if (!logz4::parse_level(word, level)) {
            reply("log: unknown level '{}'", word);
            return 1;
        }
        logz4::set_min_level(level);
        return 0;
    }

    auto *const domain = find_domain(values.get<std::string_view>("domain"));
    if (domain == nullptr) {
        return 1;
    }
    if (word == FOLLOW_GLOBAL) {
        domain->clear_min_level();
        return 0;
    }
    auto level = logz4::Level::Info;
    if (!logz4::parse_level(word, level)) {
        reply("log: unknown level '{}'", word);
        return 1;
    }
    domain->set_min_level(level);
    return 0;
}

auto log_command(gktl::Invocation const &invocation) -> int {
    auto options = gktl::Options{"log"};
    options.add<std::string_view>("verb").choices(VERBS).help(
        "dump the record ring, domains lists them, level moves a floor");
    options.add<std::string_view>("domain").help("the domain the verb works on");
    options.add<std::string_view>("level").help(
        "the floor to set, or default to follow the global");
    auto const parsed = options.parse(invocation);
    if (parsed.is_err()) {
        gktl::reject(options, parsed.unwrap_err());
        return 1;
    }
    auto const values = parsed.unwrap();

    // Bare `log` keeps meaning "dump everything": it is what panic triage
    // and the older notes in docs/ type.
    if (!values.has("verb")) {
        logz4::dump_recent();
        return 0;
    }
    auto const verb = values.get<std::string_view>("verb");
    if (verb == "dump" && !values.has("level")) {
        return dump(values);
    }
    if (verb == "domains" && !values.has("domain") && !values.has("level")) {
        return list_domains();
    }
    if (verb == "level" && values.has("level")) {
        return set_level(values);
    }
    // The line was accepted but the words do not compose into one of the
    // verb's shapes; the usage lines say which words each one takes.
    reply("usage: log [dump [domain=<d>] | verb=domains | verb=level [domain=<d>] "
          "level=<level|default>]");
    return 1;
}
} // namespace

GKTL_COMMAND(LogDump, "log", log_command,
             "log [verb=<dump|domains|level>] [domain=<d>] [level=<level|default>]: dump the "
             "record ring, list domains, or set the global or a domain's floor")
