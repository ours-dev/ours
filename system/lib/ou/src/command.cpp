/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/command.hpp>

#include <ours/vdso.hpp>

namespace ou {
auto command_list(char *const buffer, usize const capacity) -> ours::Status {
    // A forwarder with nothing to add, and that is the point rather than an
    // omission: this library is where a program looks up *how* to reach a
    // system call, so an entry that needs no wrapping still needs a name here.
    // A program that called ours_command_list() directly would be reaching
    // past the only place that is allowed to know what the raw ABI looks like.
    return ours_command_list(buffer, capacity);
}
} // namespace ou
