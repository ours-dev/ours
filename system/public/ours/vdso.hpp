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

#include <ours/object_info.hpp>
#include <ours/port.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

/// The vDSO's exported entry points: the system-call API a program calls.
///
/// A caller declares nothing and looks up nothing: it calls
/// ours::debug_write() the way it would call any function. The linker records
/// an undefined symbol. Whoever loads the program resolves it against the
/// vDSO's .dynsym before the program runs (elfload::Binder, from the kernel's
/// userabi::launch() or the user-space launcher in //system/lib/launch), and
/// the call lands on the stub that traps. That is the same arrangement
/// Zircon's programs have with libzircon.so, and the same one a phys module
/// has with physload one layer down.
///
/// The name a program writes and the symbol it binds to differ, on purpose.
/// The symbol is `ours_<name>`: binding is by name, so the symbols are the
/// ABI, and they are plain C names because a mangled name is one compiler's
/// detail. The name in C++ is `ours::<name>`, beside the types the calls take
/// (ours::Status, ours::Uandle). Each declaration carries an asm label,
/// `__asm__("ours_<name>")`, which sets the symbol a function's calls refer
/// to; without it, `ours::debug_write` would refer to its mangled name, which
/// the vDSO does not export, and `extern "C"` would drop the namespace from
/// the symbol instead (`debug_write`). The label is a GNU extension, which
/// clang and GCC both honour; the header is C++ only already. Zircon's API is
/// C, so its names and symbols are both `zx_<name>`.
namespace ours {
// One declaration per entry of //system/public/ours/syscalls.gni, with that
// entry's documentation: generated, like the stubs they bind to, so that the
// two cannot disagree about a call's arguments (design 4.7).
#include <ours/vdso_decls.inc>
} // namespace ours

#endif // __cplusplus

#endif // OURS_VDSO_HPP
