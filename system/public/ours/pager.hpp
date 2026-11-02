/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PAGER_HPP
#define OURS_PAGER_HPP 1

// The pager calls, one domain in one file: a program that supplies the pages
// of a VMO the way a file system supplies a file's includes this and nothing
// else of the system-call surface.

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vmo.hpp>

namespace ours {
// The pager calls, as a program calls them: generated from
// //system/public/ours/syscalls/pager.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/pager.inc>
} // namespace ours

#endif // OURS_PAGER_HPP
