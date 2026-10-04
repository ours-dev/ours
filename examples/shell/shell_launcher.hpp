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
/// The library launcher (U7), for an image where this shell holds VMEX.
using Launcher = launch::BootfsLauncher;

/// How the shell starts its commands, whichever way it was given:
///   - |connection|, a connection to the launcher service: the dev image,
///     where init started this shell and only the launcher holds VMEX;
///   - |library|, made with this shell's own VMEX: the dbg_1 image, where
///     userboot started this shell and there is no launcher service.
/// |bootfs_file| is the BootFS mapped read-only, for `ls`, either way.
struct Starter {
    Launcher library;
    ours::Uandle connection = ours::Uandle::Invalid;
    std::span<u8 const> bootfs_file;

    [[nodiscard]]
    auto can_start() const -> bool {
        return connection != ours::Uandle::Invalid || library.executable != ours::Uandle::Invalid;
    }
};

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
