/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_LEVEL_HPP
#define LOGZ4_LEVEL_HPP 1

#include <string_view>

#include <ours/types.hpp>

namespace logz4 {

/// Severity levels, ordered: a bigger value is more severe, so "is this
/// level enabled" is a single comparison and no rank remap. Trace is the
/// most verbose and is compiled out of release-grade images entirely (see
/// filter.hpp).
enum class Level : u8 {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

[[nodiscard]]
constexpr auto level_name(Level level) -> char const * {
    switch (level) {
        case Level::Trace:
            return "trace";
        case Level::Debug:
            return "debug";
        case Level::Info:
            return "info";
        case Level::Warn:
            return "warn";
        case Level::Error:
            return "error";
    }
    return "?";
}

/// Maps a boot-option / kshell spelling onto a Level. Unknown names fail
/// rather than defaulting: a typo in `log.level=` is a malformed command
/// line, not a silent Info.
[[nodiscard]]
constexpr auto parse_level(std::string_view name, Level &out) -> bool {
    if (name == "trace") {
        out = Level::Trace;
        return true;
    }
    if (name == "debug") {
        out = Level::Debug;
        return true;
    }
    if (name == "info") {
        out = Level::Info;
        return true;
    }
    if (name == "warn") {
        out = Level::Warn;
        return true;
    }
    if (name == "error") {
        out = Level::Error;
        return true;
    }
    return false;
}

} // namespace logz4

#endif // LOGZ4_LEVEL_HPP
