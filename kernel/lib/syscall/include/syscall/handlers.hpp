/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SYSCALL_HANDLERS_HPP
#define SYSCALL_HANDLERS_HPP 1

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <user_copy/user_copy.hpp>

namespace ours {
/// The typed system-call entry points.
///
/// These take arguments and return a Status like ordinary functions. Nothing
/// about a register frame reaches them: a generated shim per call
/// (syscall_abi/kernel_shims.inc, included by table.cpp) pulls the arguments
/// out of arch::SyscallFrame and puts the Status back into it. That shim is
/// the dispatch layer, and keeping it separate is what lets these be read,
/// reviewed and eventually tested as plain functions -- the same split Zircon
/// draws between its generated syscall wrappers and the sys_* functions they
/// call.
///
/// A pointer parameter here is a user_copy::UserPtr, not a C++ pointer. The
/// type is the rule: it cannot be dereferenced, only copied through, and every
/// copy checks the range and opens the SMAP window. The direction is part of
/// the type, so writing through an input is a compile error.

// The declarations are generated from //system/public/ours/syscalls.gni
// (design 4.7), one per entry. The definitions are written by hand, one per
// file under src/, and that split is the check: a definition whose
// signature has drifted from its entry is a different overload that nothing
// calls, and the generated shim's call to the declared one fails to link.
#include <syscall_abi/kernel_decls.inc>

} // namespace ours

#endif // SYSCALL_HANDLERS_HPP
