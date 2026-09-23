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
/// Writes |length| bytes of |buffer| to the kernel's debug console.
///
/// A debug facility, not a console: it exists so a program can prove it
/// reached ring 3 and got back. The kernel caps one call's length (see
/// MAX_DEBUG_WRITE in kernel/lib/syscall/src/debug.cpp) rather than spending
/// unbounded time in the kernel on a user program's say-so, and answers
/// OutOfRange past it instead of truncating.
auto ours_debug_write(char const *buffer, usize length) -> ours::Status;

/// Writes the kshell command registry into |buffer| as text, one command per
/// line, NUL terminated; NoSpace when the registry does not fit.
auto ours_command_list(char *buffer, usize length) -> ours::Status;

/// Closes |handle|. HANDLE_INVALID is accepted and ignored; any other value
/// that names nothing in the caller's table answers BadHandle.
auto ours_handle_close(ours::HandleValue handle) -> ours::Status;

/// Makes a channel and writes a handle to each end into |out0| and |out1|.
auto ours_channel_create(ours::ChannelCreateOptions options, ours::HandleValue *out0,
                         ours::HandleValue *out1) -> ours::Status;

/// Queues |num_bytes| of |bytes| on the channel's other end. Handles in a
/// message are not supported yet: |num_handles| must be 0.
auto ours_channel_write(ours::HandleValue channel, ours::ChannelWriteOptions options,
                        void const *bytes, u32 num_bytes, ours::HandleValue const *handles,
                        u32 num_handles) -> ours::Status;

/// Takes the next message into |bytes|. ShouldWait when there is none yet,
/// PeerClosed when there is none and never will be, BufferTooSmall (with the
/// sizes in |actual_*|) when it does not fit.
auto ours_channel_read(ours::HandleValue channel, ours::ChannelReadOptions options, void *bytes,
                       ours::HandleValue *handles, u32 num_bytes, u32 num_handles,
                       u32 *actual_bytes, u32 *actual_handles) -> ours::Status;

/// Waits until the object asserts any of |signals|, the handle is closed, or
/// |deadline| passes (0 polls; INSTANT_INFINITE waits for ever), and writes
/// what was asserted into |observed|.
auto ours_object_wait_one(ours::HandleValue handle, ours::Signals signals, ours::Instant deadline,
                          ours::Signals *observed) -> ours::Status;
}

#endif // __cplusplus

#endif // OURS_VDSO_HPP
