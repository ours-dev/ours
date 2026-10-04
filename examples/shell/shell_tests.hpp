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
} // namespace shell::tests

#endif // OURS_EXAMPLES_SHELL_SHELL_TESTS_HPP
