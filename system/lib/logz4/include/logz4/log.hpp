/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_LOG_HPP
#define LOGZ4_LOG_HPP 1

#include <logz4/filter.hpp>
#include <logz4/level.hpp>
#include <logz4/logger.hpp>

#include <ours/compiler.hpp>
#include <ustl/fmt/make_format_args.hpp>
#include <ustl/util/move.hpp>
#include <ustl/views/string_view.hpp>

namespace logz4 {
/// Last-resort output, used only when an Error-severity line arrives and no
/// logger has been installed yet (early panic before console bring-up).
/// Default: drops the bytes. Architectures with a known-good boot port
/// override this with a strong definition (e.g. direct COM1 polling).
auto emergency_write(char const *s, usize n) -> void;

/// Installs `logger` as the global logger. Safe to call again to replace
/// the current one; a null pointer is ignored, leaving the current logger
/// installed.
PHYS_EXPORT
auto init(Logger *logger) -> void;

PHYS_EXPORT
auto get_global_logger() -> Logger &;

PHYS_EXPORT
auto set_global_logger(Logger &logger) -> void;

/// Replay the record ring onto the current logger, or onto emergency_write
/// when nothing is installed yet. Panic and the kshell `log` command both
/// come through here so a dump always uses the same routing as a live line.
PHYS_EXPORT
auto dump_recent() -> void;

// The one cross-module entry point: physload exports this plain function so
// physload_module images bind their log calls to physload's single global
// logger instead of growing a disconnected copy. (A template cannot play
// this role: -fvisibility=hidden makes implicit-instantiation references
// hidden, and a hidden undefined reference can never bind to the loader's
// export table.)
PHYS_EXPORT
auto vlog_dispatch(Logger &logger, Level level, ustl::views::StringView fmt,
                   ustl::fmt::FormatArgs const &args) -> void;

/// Formats `fmt`/`args` into one whole line (metadata prefix, body,
/// newline), appends it to the record ring, and hands it to `logger` in a
/// single write() call. Defined in log.cpp for ustl::views::StringView;
/// every caller in the tree passes exactly that type.
template <typename String>
auto vlog(Logger &logger, Level level, String fmt, ustl::fmt::FormatArgs const &args) -> void;

template <typename... Args>
inline auto log(Level level, ustl::views::StringView fmt, Args &&...args) -> void {
    // Each argument goes through decay_format_arg() and the lambda binds the
    // results as lvalues, which is what make_format_args() requires -- the
    // decay may hand back a prvalue string_view, and it has to outlive the
    // argument store.
    //
    // Without the decay, a `char buf[N]` argument reaches the store as an
    // array type and this libc++ formats it as nothing at all: `log::info("|{}|",
    // name)` printed "||" for a name that was perfectly well formed. It fails
    // silently, which makes it a bad thing for a logger to do -- the one line
    // you added to find a bug is the line that lies to you.
    return [&](auto &&...prepared) {
        vlog_dispatch(get_global_logger(), level, fmt, ustl::fmt::make_format_args(prepared...));
    }(ustl::fmt::decay_format_arg(ustl::forward<Args>(args))...);
}

// The Debug/Trace levels are compiled out unless the image was built with
// the debug level turned up (KERNEL_DEBUG_LEVEL, defined by every kernel and
// phys switchset). This keeps the runtime-visible noise floor identical to
// what the old phys dprintln() gating produced, and keeps hot paths such as
// the fault handlers free of any logging cost in release images.
#if defined(KERNEL_DEBUG_LEVEL) && KERNEL_DEBUG_LEVEL
#define LOGZ4_VERBOSE_LOGGING 1
#else
#define LOGZ4_VERBOSE_LOGGING 0
#endif

template <typename... Args>
inline auto debug(ustl::views::StringView fmt, Args &&...args) -> void {
    if constexpr (!LOGZ4_VERBOSE_LOGGING) {
        return;
    }
    return logz4::log(Level::Debug, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto info(ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log(Level::Info, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto trace(ustl::views::StringView fmt, Args &&...args) -> void {
    if constexpr (!LOGZ4_VERBOSE_LOGGING) {
        return;
    }
    return logz4::log(Level::Trace, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto warn(ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log(Level::Warn, fmt, ustl::forward<Args>(args)...);
}

template <typename... Args>
inline auto error(ustl::views::StringView fmt, Args &&...args) -> void {
    return logz4::log(Level::Error, fmt, ustl::forward<Args>(args)...);
}

} // namespace logz4

///
namespace log = logz4;

#endif // LOGZ4_LOG_HPP
