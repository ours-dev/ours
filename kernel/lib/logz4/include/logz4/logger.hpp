/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef LOGZ4_LOGGER_HPP
#define LOGZ4_LOGGER_HPP 1

#include <logz4/level.hpp>
#include <ustl/fmt/format_args.hpp>
#include <ustl/source_location.hpp>
#include <ustl/views/string_view.hpp>

namespace logz4 {
struct Record {
    Level level;
    ustl::fmt::FormatArgs args;
    ustl::views::StringView fmtstr;
    ustl::SourceLocation location;
};

class Logger {
    using Self = Logger;

  public:
    Logger() = default;

    virtual ~Logger() = default;

    virtual auto is_enabled() -> bool {
        return true;
    }

    virtual auto log(Record const &record) -> void = 0;

    virtual auto flush() -> void = 0;

    virtual auto kill() -> void = 0;
};

} // namespace logz4

#endif // #ifndef LOGZ4_LOGGER_HPP