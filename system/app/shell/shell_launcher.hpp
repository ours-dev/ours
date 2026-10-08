/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SHELL_SHELL_LAUNCHER_HPP
#define SHELL_SHELL_LAUNCHER_HPP

#include <ours/process_args.hpp>
#include <ours/types.hpp>

#include <span>
#include <string_view>

#include <launch/bootfs.hpp>
#include <launch/launch.hpp>

namespace shell {
/// The library launcher (U7), for an image where this shell holds VMEX.
using Launcher = launch::BootfsLauncher;

/// How the shell starts its commands, told apart by what it was handed
/// (component-framework design, "Events: dev.dbg_1"):
///   - |component|: the dev image, where the root started this shell as a
///     component, with a namespace that holds /boot and /svc. A command is
///     /boot/bin/<name>, spawned through the root's ours.process.Launcher
///     (fsio::spawn(), design D12), and `ls` lists a namespace directory;
///   - |library|, made with this shell's own VMEX: the dbg_1 image, where
///     userboot started this shell with the BootFS and VMEX and there is no
///     root. A command is a BootFS name, and |bootfs_file|, the BootFS
///     mapped read-only, is what `ls` lists.
struct Starter {
    Launcher library;
    bool component = false;
    std::span<u8 const> bootfs_file;

    [[nodiscard]]
    auto can_start() const -> bool {
        return component || library.executable != ours::Uandle::Invalid;
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

#endif // SHELL_SHELL_LAUNCHER_HPP
