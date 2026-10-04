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

    /// A connection to the launcher service, where init started the shell.
    ours::Uandle connection;
};

auto run(Inputs const &inputs) -> void;

/// The Loop (//system/lib/loop) on real channels and a real port: two
/// channels served in turns, re-arming, a callback closing its own slot, and
/// packets that name no slot in use dropped. Prints "shell: loop checks
/// passed" or "FAILED".
///
/// Not called from run() yet: it passes only once the user has written
/// Loop::run() by hand (P3 of .agents/tasks/component-framework/PLAN.md),
/// and until then it would print FAILED, which boot_check refuses. Wiring it
/// is one line in run(), `check_loop();` after check_port_observer_limit().
auto check_loop() -> void;
} // namespace shell::tests

#endif // OURS_EXAMPLES_SHELL_SHELL_TESTS_HPP
