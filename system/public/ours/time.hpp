/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_HPP
#define OURS_TIME_HPP 1

// The clock calls, one domain in one file: what a program reads the passage
// of time with. The vDSO answers these itself where it can -- no trap for a
// clock read that a page of published data can serve (design D8).

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
// The clock calls, as a program calls them: generated from
// //system/public/ours/syscalls/time.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/time.inc>
} // namespace ours

#endif // OURS_TIME_HPP
