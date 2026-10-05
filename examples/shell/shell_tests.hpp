/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_EXAMPLES_SHELL_SHELL_TESTS_HPP
#define OURS_EXAMPLES_SHELL_SHELL_TESTS_HPP

#include "shell_launcher.hpp"

namespace shell::tests {
struct Inputs {
    /// The job this shell was handed: the root job from userboot (dbg_1), a
    /// duplicate of it from the root (dev). run() closes it as its
    /// check_handle_close(), after the checks that start processes in it.
    ours::Uandle job;
    ours::Uandle bootfs;
    ours::Uandle vmex;
    ours::Uandle vdso;
    ours::Uandle vma;
    /// The SAMPLING resource, where userboot started this shell (dbg_1);
    /// Uandle::Invalid on dev, where the root holds it.
    ours::Uandle sampling;
    shell::LaunchProgram launch_program;
    void *launcher_context;
    /// The library launcher where this shell has one (dbg_1), or nullptr:
    /// check_start_needs_execute() calls launch::start() with its vDSO and
    /// this process's region.
    shell::Launcher const *library;

    /// Started by the root as a component (dev): its namespace holds /boot
    /// and /svc, which the Launcher checks and fsio::spawn()'s check go
    /// through.
    bool component;
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
