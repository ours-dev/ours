/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_EXAMPLES_SHELL_SHELL_TESTS_HPP
#define OURS_EXAMPLES_SHELL_SHELL_TESTS_HPP

#include "shell_launcher.hpp"

namespace shell::tests {
struct Inputs {
    ours::Uandle peer;
    ours::Uandle job;
    ours::Uandle bootfs;
    ours::Uandle vmex;
    ours::Uandle vdso;
    ours::Uandle vma;
    ours::Uandle startup_to_close;
    shell::LaunchProgram launch_program;
    void *launcher_context;
    /// The library launcher where this shell has one (dbg_1), or nullptr:
    /// check_start_needs_execute() calls launch::start() with its vDSO and
    /// this process's region.
    shell::Launcher const *library;

    /// A connection to the launcher service, where init started the shell.
    ours::Uandle connection;
};

auto run(Inputs const &inputs) -> void;

/// The Loop (//system/lib/loop) on real channels and a real port: two
/// channels served in turns, re-arming, a callback closing its own slot, and
/// packets that name no slot in use dropped. Prints "shell: loop checks
/// passed" or "FAILED". Called from run() on both images, after
/// check_port_observer_limit().
auto check_loop() -> void;
} // namespace shell::tests

#endif // OURS_EXAMPLES_SHELL_SHELL_TESTS_HPP
