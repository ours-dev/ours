/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_SYSCALL_HPP
#define OURS_SYSCALL_HPP 1

// The constants a program and the kernel have to agree on about system calls:
// limits, sentinels and option words, each named once so that both sides
// compile against the same value.
//
// What is not here is the call numbers. They used to be, as OURS_SYS_* macros
// with a count beside them, on the grounds that "a user program puts one of
// them in the syscall-number register". No user program does: the only
// `syscall` instruction in a user address space is in the vDSO, and the kernel
// refuses a trap from anywhere else. A program names a call by its vDSO symbol
// (<ours/vdso.hpp>), so the numbers are a private agreement between the vDSO
// and the kernel's dispatch table (design D8). Both are now generated from
// //system/public/ours/syscalls.gni, into a header only those two can see
// (//system/public:syscalls.private). Publishing the numbers would only have
// invited the one thing the vDSO exists to prevent.

#include <ours/types.hpp>

#include <limits>

namespace ours {
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

/// The most handles one channel message may carry (Zircon's number).
inline constexpr auto CHANNEL_MAX_MSG_HANDLES = u32{64};

/// The most live handles one process may have created, wherever they are now:
/// in its own table, in another process's, or queued in a message on the
/// way. Creating one past it answers NoResource; closing one gives the room
/// back.
///
/// Counted against the creator, not the holder, so that sending handles
/// away does not make room for more: a process that makes channels and
/// writes their ends into a channel nobody reads would otherwise fill the
/// system-wide arena from a table that always looks empty. A quarter of
/// today's arena, so that no one process can exhaust it alone; it grows when
/// the arena does.
inline constexpr auto HANDLE_MAX_PER_PROCESS = u32{1024};

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
} // namespace ours

#endif // OURS_SYSCALL_HPP
