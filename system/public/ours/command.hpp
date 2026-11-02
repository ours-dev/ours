/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_COMMAND_HPP
#define OURS_COMMAND_HPP 1

// The kernel command registry's call, one domain in one file: a program that
// lists what the kernel can be asked includes this and nothing else of the
// system-call surface.

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
// The command calls, as a program calls them: generated from
// //system/public/ours/syscalls/command.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/command.inc>
} // namespace ours

#endif // OURS_COMMAND_HPP
