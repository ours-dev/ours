/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `log`: dump logz4's record ring, list the log targets, or move a level
// floor -- the global one or a single target's. The logging library itself
// stays free of any dependency on gktl's command registry -- phys links
// //system/lib/logz4 and has no console at all.
#include <format>
#include <string_view>

#include <gktl/command.hpp>
#include <logz4/filter.hpp>
#include <logz4/level.hpp>
#include <logz4/log.hpp>
#include <logz4/target.hpp>
#include <ustl/fmt/format_to.hpp>

namespace {
/// A reply to whoever typed the command, written straight to the console
/// sink as one line.
///
/// Not a log line: it would pass through the very floors this command sets,
/// so `log level error` followed by `log targets` would print nothing, and
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

/// `default` hands a target back to the global floor. A word rather than a
/// level name because it is not a level: the target stops having one.
constexpr std::string_view FOLLOW_GLOBAL = "default";

auto find_target(char const *name) -> logz4::Target * {
    auto *const target = logz4::Target::find(name);
    if (target == nullptr) {
        reply("log: no target '{}' (see `log targets`)", name);
    }
    return target;
}

auto parse_level(char const *name, logz4::Level &level) -> bool {
    if (!logz4::parse_level(name, level)) {
        reply("log: unknown level '{}'", name);
        return false;
    }
    return true;
}

/// `log dump [<target>]`
auto dump(int argc, char **argv) -> int {
    if (argc == 2) {
        logz4::dump_recent();
        return 0;
    }
    auto const *const target = find_target(argv[2]);
    if (target == nullptr) {
        return 1;
    }
    logz4::dump_recent(*target);
    return 0;
}

/// `log targets`: every target in the image with the floor it filters at.
auto list_targets() -> int {
    reply("log: global floor {}", logz4::level_name(logz4::min_level()));
    for (auto const &target: logz4::Target::all()) {
        if (auto const own = target.own_min_level(); own.has_value()) {
            reply("  {:<12} {}", target.name(), logz4::level_name(*own));
        } else {
            reply("  {:<12} {} ({})", target.name(), logz4::level_name(target.min_level()),
                  FOLLOW_GLOBAL);
        }
    }
    return 0;
}

/// `log level <level>` moves the global floor; `log level <target> <level>`
/// gives one target its own, and `log level <target> default` takes it away.
/// The argument count tells the two apart, so a target that happens to be
/// called "warn" is still reachable.
auto set_level(int argc, char **argv) -> int {
    auto level = logz4::Level::Info;
    if (argc == 3) {
        if (!parse_level(argv[2], level)) {
            return 1;
        }
        logz4::set_min_level(level);
        return 0;
    }

    auto *const target = find_target(argv[2]);
    if (target == nullptr) {
        return 1;
    }
    if (argv[3] == FOLLOW_GLOBAL) {
        target->clear_min_level();
        return 0;
    }
    if (!parse_level(argv[3], level)) {
        return 1;
    }
    target->set_min_level(level);
    return 0;
}

auto log_command(int argc, char **argv) -> int {
    // Bare `log` keeps meaning "dump everything": it is what panic triage
    // and the older notes in docs/ type.
    if (argc == 1) {
        logz4::dump_recent();
        return 0;
    }

    auto const verb = std::string_view(argv[1]);
    if (verb == "dump" && argc <= 3) {
        return dump(argc, argv);
    }
    if (verb == "targets" && argc == 2) {
        return list_targets();
    }
    if (verb == "level" && (argc == 3 || argc == 4)) {
        return set_level(argc, argv);
    }
    reply("usage: log [dump [<target>] | targets | level [<target>] <level>|default]");
    return 1;
}
} // namespace

GKTL_COMMAND(LogDump, "log", log_command,
             "log [dump [<target>] | targets | level [<target>] <level>|default]: "
             "dump the record ring, list targets, or set the global or a target's floor")
