/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_VDSO_HPP
#define OURS_VDSO_HPP 1

// The contract between the vDSO, whoever maps it into a process, and the code
// that calls through it.
//
// The vDSO is the only image whose `syscall` the kernel accepts. A process
// whose `syscall` instruction does not lie wholly inside the range its vDSO
// mapping registered is killed (kernel/lib/syscall/src/table.cpp,
// trapped_from_vdso()). A user program therefore has no reason to trap
// directly (one that does is killed, as fault_probe's trap.S shows on
// purpose); it calls a vDSO entry, which traps on its behalf.
//
// This file used to also describe an export table: a magic number, a version,
// and a byte offset per entry, which every program copied into itself and
// resolved by hand. That table is gone. A program calls the functions declared
// below and the linker leaves them undefined. Whoever loads the program binds
// them to the vDSO's .dynsym before it runs (elfload::Binder, from the kernel's
// userabi::launch() or the user-space launcher), so the declarations *are* the
// contract now, with nothing to keep in step by hand.

#ifdef __cplusplus

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

/// The vDSO's exported entry points, as //system/lib/ou calls them (and,
/// until system-abi-surface step 5, examples/shell directly).
///
/// A caller declares nothing and looks up nothing: it calls ours_debug_write()
/// the way it would call any function. The linker records an undefined
/// symbol. Whoever loads the program resolves it against the vDSO's .dynsym
/// before the program runs (elfload::Binder, from the kernel's
/// userabi::launch() or the user-space launcher in //system/lib/launch), and
/// the call lands on the stub that traps. That is the same arrangement
/// Zircon's programs have with libzircon.so, and the same one a phys module
/// has with physload one layer down.
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
