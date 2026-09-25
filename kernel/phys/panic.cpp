/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/panic.hpp>

#include <logz4/log.hpp>
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
auto vpanic(std::string_view message) -> void {
    // The message arrives pre-rendered by panic()'s bounded stack buffer;
    // logz4 routes it to the console logger, or to the serial emergency
    // path when the logger is not up yet.
    //
    // It goes over as an argument of a "{}" format string, not as the format
    // string: text that has already been through the formatter must not be
    // parsed by it a second time. See the kernel's vpanic() for what that
    // cost.
    logz4::dump_recent();
    log::vlog(logz4::get_global_logger(), logz4::Level::Error, std::string_view("{}"),
              ustl::fmt::make_format_args(message));
    while (1)
        ;
}
} // namespace ours
