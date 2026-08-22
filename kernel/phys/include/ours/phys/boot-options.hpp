/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_BOOT_OPTIONS_HPP
#define OURS_PHYS_BOOT_OPTIONS_HPP 1

#include <obi/command-line-view.hpp>
#include <ours/compiler.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::phys {
struct BootOptions {
    // Keep the current debug baseline explicit; `aslr.disabled=0` in the
    // OBI command line opts into KASLR for every boot entry point.
    bool aslr_disabled{true};

    /// The phys module physload loads next (`kernel.phys.next=<name>`);
    /// a BootFS file name inside the kernel package.  The default is the
    /// physboot module — writing `kernel.phys.next=physboot` explicitly is
    /// equivalent to omitting the option.
    char next_module[64]{"physboot"};

    /// The kernel image directory physboot boots (`kernel.name=<dir>`),
    /// resolved inside the same kernel-package BootFS.  File names inside
    /// the directory are fixed vocabulary owned by obi::KernelPackage
    /// (kImageElf / kCodePatches), not boot parameters.
    char kernel_name[64]{"vmours"};
};

/// Apply the subset of OBI cmdline options consumed before the kernel is
/// loaded. Unknown tokens are intentionally left for later boot stages.
PHYS_EXPORT auto apply_boot_options(BootOptions &options, obi::CommandLineView const &cmdline) -> Status;

/// physload owns the parsed options; modules bind to this object.
PHYS_EXPORT extern BootOptions g_boot_options;

inline auto global_boot_options() -> BootOptions * {
    return &g_boot_options;
}

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_BOOT_OPTIONS_HPP
