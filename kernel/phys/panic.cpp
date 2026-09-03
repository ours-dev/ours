/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <logz4/log.hpp>
#include <ours/panic.hpp>
#include <ustl/fmt/make_format_args.hpp>

namespace ours {
[[noreturn]]
auto panic() -> void {
    logz4::dump_recent();
    log::error("panic");
    while (1)
        ;
}

[[noreturn]]
auto vpanic(std::string_view fmt) -> void {
    // The message arrives pre-rendered by panic()'s bounded stack buffer;
    // logz4 routes it to the console logger, or to the serial emergency
    // path when the logger is not up yet.
    logz4::dump_recent();
    log::vlog(logz4::get_global_logger(), logz4::Level::Error, fmt, ustl::fmt::make_format_args());
    while (1)
        ;
}
} // namespace ours
