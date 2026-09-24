/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYSCALL_HPP
#define OURS_SYSCALL_HPP 1

// The system-call ABI: the contract between a user program and the kernel's
// entry stub.  It lives in system/public because both sides have to agree on
// it -- the kernel builds its dispatch table from these numbers, and a user
// program puts one of them in the syscall-number register.
//
// Written so an assembler can read it.  kernel/arch/x86/kernel/syscall.S needs
// OURS_SYSCALL_COUNT for its range check, and the assembler will not survive a
// C++ declaration, so the count is a macro and the C++ spelling below is a
// mirror rather than the definition.

/// How many system-call numbers exist.  Numbers are dense from zero, so this
/// is also one past the largest valid number.
///
/// The dispatch table is deliberately one entry LONGER than this: index
/// OURS_SYSCALL_COUNT is a sentinel slot holding the out-of-range answer, and
/// the entry stub clamps a user-supplied number onto it rather than branching
/// around the call.  See SYSCALL_TABLE in //kernel/lib/syscall/table.cpp.
#define OURS_SYSCALL_COUNT 256

/// Write a string to the kernel's debug console.
///
/// No user program issues these numbers itself: the only `syscall`
/// instruction in a user address space is in the vDSO, and the kernel refuses
/// a trap from anywhere else. The register convention below is therefore the
/// contract between the vDSO stubs and the kernel, not something a program
/// sees -- a program calls ours_debug_write() (<ours/vdso.hpp>).
///
/// Arguments ride in the x86-64 SysV argument registers, so that a stub is
/// mostly the trap itself: %rdi is the buffer, %rsi its length in bytes. %rax
/// carries the number in and the ours::Status out. %rcx and %r11 are NOT
/// arguments and never can be: the SYSCALL instruction overwrites both before
/// the kernel sees them, which is why the fourth argument travels in %r10
/// (the stub moves it there from %rcx), and the seventh and eighth -- which
/// SysV puts on the stack -- in %r12 and %r13.
///
/// This is a debug facility, not a console: it exists so that the first user
/// program can prove it reached user mode and got back, and it has no
/// counterpart in a finished system.  Zircon's zx_debug_write is the same
/// thing for the same reason.
#define OURS_SYS_DEBUG_WRITE 0

/// ours_command_list(char *buffer, usize length) -> Status
///
/// Writes the kshell command registry into |buffer| as text, one command per
/// line, NUL terminated. It reads the registry and copies bytes out; it does
/// not run anything, which is why it can exist before this kernel has a
/// copy_to_user worth the name.
#define OURS_SYS_COMMAND_LIST 1

/// ours_handle_close(HandleValue handle) -> Status
///
/// Takes |handle| out of the caller's handle table; when it was the last
/// handle to its object, the object learns it has no holders left. Closing
/// HANDLE_INVALID succeeds and does nothing. No right is needed to give a
/// handle up.
#define OURS_SYS_HANDLE_CLOSE 2

/// ours_channel_create(ChannelCreateOptions options, HandleValue *out0,
///                     HandleValue *out1) -> Status
///
/// Makes both ends of a new channel and puts a handle to each in the caller's
/// table. Both values are written out before either handle is installed, so
/// a bad out-pointer leaves nothing behind.
#define OURS_SYS_CHANNEL_CREATE 3

/// ours_channel_write(HandleValue channel, ChannelWriteOptions options,
///                    void const *bytes, u32 num_bytes,
///                    HandleValue const *handles, u32 num_handles) -> Status
///
/// Six arguments: the fourth travels in %r10.
#define OURS_SYS_CHANNEL_WRITE 4

/// ours_channel_read(HandleValue channel, ChannelReadOptions options,
///                   void *bytes, HandleValue *handles, u32 num_bytes,
///                   u32 num_handles, u32 *actual_bytes,
///                   u32 *actual_handles) -> Status
///
/// Eight arguments: the fourth travels in %r10, the seventh and eighth -- on
/// the stack under SysV -- in %r12 and %r13.
#define OURS_SYS_CHANNEL_READ 5

/// ours_object_wait_one(HandleValue handle, Signals signals, Instant deadline,
///                      Signals *observed) -> Status
#define OURS_SYS_OBJECT_WAIT_ONE 6

/// ours_process_spawn_boot(HandleValue job, char const *name, usize name_size,
///                         HandleValue bootstrap) -> Status
///
/// TEMPORARY. Starts the program |name| from the kernel package as a new
/// process under |job|, handing it |bootstrap|. It exists only until a user
/// space loader can build a process out of VMOs and VMARs (design P1-C), and
/// goes with it; see docs/repairs/260923/syscall.md.
#define OURS_SYS_PROCESS_SPAWN_BOOT 7

#ifdef __cplusplus

#include <ours/types.hpp>

#include <limits>

namespace ours {
/// The C++ spelling of OURS_SYSCALL_COUNT above.
inline constexpr auto SYSCALL_COUNT = usize{OURS_SYSCALL_COUNT};

/// A deadline that never arrives: wait until the condition holds, however
/// long that takes (Zircon ZX_TIME_INFINITE).
///
/// Its own constant rather than the kernel's INFINITE_TIME, which is a chrono
/// TimePoint the kernel's timers are written against. This one is the plain
/// number a program passes in a register.
inline constexpr auto INSTANT_INFINITE = std::numeric_limits<Instant>::max();

/// The most bytes one channel message may carry.
///
/// 4 KiB, which is the largest single kmalloc: a message is copied into one
/// kernel allocation before the channel's lock is taken, and that allocation
/// does not go through vmalloc, whose 256 regions are shared by the whole
/// system and whose vfree has no TLB shootdown yet. Zircon allows 64 KiB;
/// raising this later is compatible, lowering it would not be.
inline constexpr auto CHANNEL_MAX_MSG_BYTES = u32{4096};

/// The most handles one channel message may carry (Zircon's number). Handles
/// in messages are not implemented yet: a write naming any answers
/// NotSupported. The limit is stated now because it is part of the shape of
/// channel_write and channel_read, not of their current implementation.
inline constexpr auto CHANNEL_MAX_MSG_HANDLES = u32{64};

/// The most bytes one sys_debug_write() will copy.
///
/// A ceiling on one call, so that a program cannot hold a CPU inside the
/// kernel for as long as it likes by asking for a gigabyte of output. The
/// number is arbitrary; what is not arbitrary is that there is one, and that
/// both sides know the same one. It lives here rather than in the handler
/// because the caller has to split its text to fit -- a limit only the kernel
/// knows is a limit the caller discovers as Status::OutOfRange.
inline constexpr auto MAX_DEBUG_WRITE = usize{256};

/// The longest program name process_spawn_boot accepts, in bytes, not
/// counting a terminator (none is passed). The kernel keeps names in 64-byte
/// buffers, as Zircon's kernel.select.userboot does.
inline constexpr auto PROCESS_BOOT_NAME_MAX = usize{63};

/// Options for the channel calls. None is the only value any of them accepts
/// today; anything else answers InvArgs rather than being ignored, so that a
/// program cannot come to depend on a bit that does nothing. They are
/// separate types, one per call, because the bits each will grow are
/// unrelated (Zircon's ZX_CHANNEL_WRITE_USE_IOVEC and ZX_CHANNEL_READ_MAY_DISCARD
/// share no meaning), and a type per call makes passing one to the other a
/// compile error.
enum class ChannelCreateOptions : u32 {
    None = 0,
};

enum class ChannelWriteOptions : u32 {
    None = 0,
};

enum class ChannelReadOptions : u32 {
    None = 0,
};

/// The C++ spelling of the numbers above.
enum class SyscallNumber : usize {
    DebugWrite = OURS_SYS_DEBUG_WRITE,
    CommandList = OURS_SYS_COMMAND_LIST,
    HandleClose = OURS_SYS_HANDLE_CLOSE,
    ChannelCreate = OURS_SYS_CHANNEL_CREATE,
    ChannelWrite = OURS_SYS_CHANNEL_WRITE,
    ChannelRead = OURS_SYS_CHANNEL_READ,
    ObjectWaitOne = OURS_SYS_OBJECT_WAIT_ONE,
    ProcessSpawnBoot = OURS_SYS_PROCESS_SPAWN_BOOT,
};
} // namespace ours

#endif // __cplusplus

#endif // OURS_SYSCALL_HPP
