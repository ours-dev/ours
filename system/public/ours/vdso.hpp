/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_VDSO_HPP
#define OURS_VDSO_HPP 1

// The contract between the vDSO, the kernel that maps it, and the program that
// calls through it.
//
// The vDSO is the only image in a user address space that contains a `syscall`
// instruction, and the kernel refuses a trap whose return address lies outside
// it (see kernel/lib/syscall/table.cpp). A user program therefore never traps
// directly; it calls a vDSO entry, which traps on its behalf.
//
// This file used to also describe an export table: a magic number, a version,
// and a byte offset per entry, which every program copied into itself and
// resolved by hand. That table is gone. A program calls the functions declared
// below, the linker leaves them undefined, and the kernel binds them to the
// vDSO's .dynsym as it loads the image -- so the declarations *are* the
// contract now, with nothing to keep in step by hand.

#ifdef __cplusplus

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

/// The vDSO's exported entry points, as a program calls them.
///
/// This is the whole user-facing surface: a program declares nothing, looks up
/// nothing, and calls ours_debug_write() the way it would call any function.
/// The linker records an undefined symbol, userboot resolves it against the
/// vDSO's .dynsym when it loads the image, and the call lands on the stub that
/// traps (kernel/lib/userabi/src/userboot.cpp, bind_imports()). That is the
/// same arrangement Zircon's programs have with libzircon.so, and the same one
/// a phys module has with physload one layer down.
///
/// `extern "C"` because binding is by name and a mangled name is an
/// implementation detail of one compiler: these names are the ABI.
extern "C" {
// One declaration per entry of //system/public/ours/syscalls.gni, with that
// entry's documentation: generated, like the stubs they bind to, so that the
// two cannot disagree about a call's arguments (design 4.7).
#include <ours/vdso_decls.inc>
}

#endif // __cplusplus

#endif // OURS_VDSO_HPP
