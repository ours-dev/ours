/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/platform/ioport.hpp>

namespace ours::platform {
/// A RISC-V CPU has no I/O port space, so the kernel keeps no port for
/// itself.
auto kernel_ioports() -> ustl::views::Span<IoPortRange const> {
    return {};
}
} // namespace ours::platform
