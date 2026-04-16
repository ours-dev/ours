/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_BOOT_OPTIONS_HPP
#define OURS_PHYS_BOOT_OPTIONS_HPP 1

#include <ours/types.hpp>

namespace ours::phys {
    struct BootOptions {
        bool aslr_disabled{false};
        char load_target[16]{"kernel_main"};
    };

    inline auto global_boot_options() -> BootOptions * {
        extern BootOptions g_boot_options;
        return &g_boot_options;
    }

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_BOOT_OPTIONS_HPP