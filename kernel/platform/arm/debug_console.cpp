/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/platform/debug_console.hpp>

namespace ours::platform {
auto debug_console_read(char *, usize const, usize *const actual) -> Status {
    *actual = 0;
    return Status::NotSupported;
}
} // namespace ours::platform