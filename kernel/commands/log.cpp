/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `log`: dump logz4's record ring, or move its runtime level floor. The
// logging library itself stays free of any dependency on gktl's command
// registry — phys links //system/lib/logz4 and has no console at all.
#include <string_view>

#include <gktl/command.hpp>
#include <logz4/filter.hpp>
#include <logz4/level.hpp>
#include <logz4/log.hpp>

namespace {
auto log_command(int argc, char **argv) -> int {
    if (argc >= 3 && std::string_view(argv[1]) == "level") {
        auto level = logz4::Level::Info;
        if (!logz4::parse_level(argv[2], level)) {
            log::warn("log: unknown level '{}'", argv[2]);
            return 1;
        }
        logz4::set_min_level(level);
        return 0;
    }
    logz4::dump_recent();
    return 0;
}
} // namespace

GKTL_COMMAND(LogDump, "log", log_command,
             "log [level <name>]: dump the record ring, or set the runtime floor")
