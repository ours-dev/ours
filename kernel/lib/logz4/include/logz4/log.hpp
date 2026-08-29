/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_LOG_HPP
#define LOGZ4_LOG_HPP 1

#include <logz4/level.hpp>
#include <logz4/logger.hpp>
#include <ustl/fmt/make_format_args.hpp>
#include <ustl/util/move.hpp>

namespace logz4 {
auto init(Logger *logger) -> void;

auto register_logger(Logger &logger) -> void;

auto unregister_logger(Logger &logger) -> void;

auto get_global_logger() -> Logger &;

auto set_global_logger(Logger &logger) -> void;

template <typename String>
auto vlog(Logger &logger, Level level, String fmt, ustl::fmt::FormatArgs const &args) -> void;

template <typename... Args>
auto log(Logger &logger, Level level, ustl::views::StringView fmt, Args &&...args) -> void {
    return vlog(logger, level, fmt, ustl::fmt::make_format_args(args...));
}

template <typename... Args>
inline auto debug(Logger &logger, ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log<Args...>(logger, Level::Debug, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto info(Logger &logger, ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log<Args...>(logger, Level::Info, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto trace(Logger &logger, ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log<Args...>(logger, Level::Trace, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto warn(Logger &logger, ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log<Args...>(logger, Level::Warn, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto error(Logger &logger, ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log<Args...>(logger, Level::Error, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
auto log(Level level, ustl::views::StringView fmt, Args &&...args) -> void {
    return log(get_global_logger(), level, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto debug(ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log<Args...>(Level::Debug, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto info(ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log<Args...>(Level::Info, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto trace(ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log<Args...>(Level::Trace, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto warn(ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log<Args...>(Level::Warn, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto error(ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log<Args...>(Level::Error, fmt, ustl::forward<Args>(args)...);
}

} // namespace logz4

///
namespace log = logz4;

#endif // #ifndef LOGZ4_LOG_HPP_
