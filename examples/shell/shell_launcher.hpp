/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_EXAMPLES_SHELL_SHELL_LAUNCHER_HPP
#define OURS_EXAMPLES_SHELL_SHELL_LAUNCHER_HPP

#include <ours/process_args.hpp>
#include <ours/types.hpp>

#include <span>
#include <string_view>

#include <launch/bootfs.hpp>
#include <launch/launch.hpp>

namespace shell {
/// The shell's launcher is the library's (U7), which init uses too.
using Launcher = launch::BootfsLauncher;

using LaunchProgram = ustl::Result<launch::Started, ours::Status> (*)(void *, ours::Uandle,
                                                                      std::string_view,
                                                                      ours::Uandle);

inline auto spells(char const *const bytes, u32 const length, std::string_view const text) -> bool {
    if (length != text.size()) {
        return false;
    }
    for (u32 index = 0; index < length; ++index) {
        if (bytes[index] != text.data()[index]) {
            return false;
        }
    }
    return true;
}
} // namespace shell

#endif // OURS_EXAMPLES_SHELL_SHELL_LAUNCHER_HPP
