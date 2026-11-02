/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IOPORT_HPP
#define OURS_IOPORT_HPP 1

// The I/O-port calls, one domain in one file: the x86 privilege that lets a
// process's threads reach a device's ports without trapping, and the resource
// that grants it.

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
// The I/O-port calls, as a program calls them: generated from
// //system/public/ours/syscalls/ioport.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/ioport.inc>
} // namespace ours

#endif // OURS_IOPORT_HPP
