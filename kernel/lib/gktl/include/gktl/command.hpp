/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_COMMAND_HPP
#define GKTL_COMMAND_HPP 1

#include <gktl/types.hpp>

namespace gktl {
struct Command {
    using Callback = auto (*)(int argc, char **argv) -> int;

    char const *cmd;
    char const *help;
    Callback callback;
};

auto run_command(int argc, char **argv) -> void;

} // namespace gktl

#define GKTL_COMMAND(NAME, CMD, HANDLER, HELP)                                                     \
    namespace commands::NAME {                                                                     \
        LINK_SECTION(".gktl.commands")                                                             \
        FORCE_USED static ::gktl::Command const COMMAND_##NAME{                                    \
            .cmd = CMD, .help = HELP, .callback = HANDLER};                                        \
    }

#endif // #ifndef GKTL_COMMAND_HPP